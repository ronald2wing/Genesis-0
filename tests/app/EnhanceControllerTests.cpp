// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The AI enhance controller's suite: drives consent -> install -> decode ->
// enhance -> encode -> apply headlessly with injected fakes, so no model
// download, no GStreamer decode/encode and no ONNX run happens in CI. The
// pinned Real-ESRGAN model's real bytes cannot be fabricated, so the installer
// seam stands in for the model store; the frame source, enhance and encode
// seams stand in for the engine and the runtime. The controller's own logic -
// the consent gate, the factor resolution, the producer/encoder handoff and the
// ReplaceClipMedia apply - runs for real against a scratch Editor.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <QCoreApplication>
#include <QObject>
#include <QString>
#include <QThread>

#include "../support/Wait.h"

#include "ai/ModelStore.h"
#include "ai/vision/EnhanceProvider.h"
#include "app/controllers/EnhanceController.h"
#include "jobs/Job.h"
#include "project/Editor.h"
#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/model/Media.h"

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const std::string &what)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

namespace ai = genesis::ai;
namespace vision = genesis::ai::vision;
using genesis::core::Rational;
using genesis::test::wait_until_idle;
using namespace genesis::project;

// A project with one video media item and one clip on the first track. The
// picture is tiny and even so the fake frame source/enhance/encode stay a
// handful of bytes while still exercising the real width/height plumbing.
struct Fixture
{
    Editor editor;
    std::string media_id;
    std::string clip_id;
};

Fixture fixture()
{
    Fixture f;
    NewMedia item;
    item.path = "/footage/a.webm";
    item.name = "a.webm";
    item.duration = Rational{ 10, 1 };
    item.kind = MediaKind::Video;
    item.width = 8;
    item.height = 8;
    const auto added = f.editor.apply(Command{ AddMedia{ std::move(item) } });
    check(added.has_value() && added->created_id.has_value(), "adds media");
    f.media_id = *added->created_id;
    const std::string track_id = f.editor.project().timelines[0].tracks[0].id;
    const auto placed =
            f.editor.apply(Command{ AddClip{ f.media_id, track_id, Rational{ 0, 1 }, false } });
    check(placed.has_value() && placed->created_id.has_value(), "adds a clip");
    f.clip_id = *placed->created_id;
    return f;
}

// The media the selected clip is cut from, or null for a dead reference.
const MediaItem *clip_media(const Fixture &f)
{
    const Clip *clip = f.editor.project().active().clip(f.clip_id);
    return clip == nullptr ? nullptr : f.editor.project().media_by_id(clip->media_id);
}

// Pumps `count` synthetic `w` x `h` RGB frames into the source's sink. The
// bytes are never read (the fake enhance ignores them), so a real decode is
// stood in for without any engine.
EnhanceController::FrameSource pump_frames(int count, int w, int h)
{
    return [count, w, h](const vision::FrameSink &sink) {
        std::vector<std::uint8_t> rgb(static_cast<std::size_t>(w) * h * 3, 0);
        for (int i = 0; i < count; ++i) {
            const vision::FrameView frame{ rgb.data(), static_cast<std::uint32_t>(w),
                                           static_cast<std::uint32_t>(h) };
            if (!sink(frame, static_cast<std::uint32_t>(i))) {
                break;
            }
        }
        vision::FrameSourceOutcome out;
        out.ok = true;
        return out;
    };
}

// An enhance seam that scales each frame to frame * factor and fills it with a
// fixed grey, so the real provider (model + runtime) is stood in for.
EnhanceController::EnhanceFn canned_enhance()
{
    return [](const vision::FrameView &frame, std::uint32_t factor, const vision::ProgressFn &,
              const vision::AbortFn &) {
        vision::EnhanceOutcome out;
        out.ok = true;
        out.width = frame.width * factor;
        out.height = frame.height * factor;
        out.rgb.assign(static_cast<std::size_t>(out.width) * out.height * 3, 0x80);
        return out;
    };
}

// An encode seam that drains the producer's frames and reports success, so no
// GStreamer encode runs. `frames` records how many frames were handed over.
EnhanceController::Encoder fake_encoder(int *frames = nullptr)
{
    return [frames](const EnhanceController::EncodeRequest &,
                    const EnhanceController::NextFrameFn &next,
                    const std::function<bool()> &cancel) {
        vision::FrameView frame;
        int count = 0;
        while (!cancel() && next(frame)) {
            ++count;
        }
        if (frames != nullptr) {
            *frames = count;
        }
        EnhanceController::EncodeOutcome out;
        out.ok = true;
        return out;
    };
}

// Points the controller's seams at fakes: a model root in a scratch dir (so the
// consent gate fires), an output root beside it, an installer that hands back a
// canned path, two synthetic frames, a fake enhancer and a fake encoder. The
// caller then overrides any seam for the scenario under test.
void wire(EnhanceController &controller, const std::filesystem::path &root)
{
    controller.setModelRoot(root);
    controller.setOutputRoot(root / "out");
    controller.setInstaller([](const std::filesystem::path &, const ai::ModelEntry &,
                               const ai::Fetcher &, const ai::Progress &) {
        ai::InstallResult result;
        result.installed = std::filesystem::path("/fake/model.onnx");
        return result;
    });
    controller.setFrameSource(pump_frames(2, 8, 8));
    controller.setEnhanceFn(canned_enhance());
    controller.setEncoder(fake_encoder());
}

// A per-run sandbox under the system temp dir, emptied first so a crashed run
// cannot leak a fresh-looking model file into the assertions.
const std::filesystem::path &sandbox()
{
    static const std::filesystem::path dir = [] {
        const std::filesystem::path p =
                std::filesystem::temp_directory_path() / "genesis-enhance-controller-tests";
        std::filesystem::remove_all(p);
        std::filesystem::create_directories(p);
        return p;
    }();
    return dir;
}

void test_status_and_availability()
{
    Fixture f = fixture();
    EnhanceController controller(&f.editor);

    const std::string status{ vision::EnhanceProvider::status() };
    check(status == "enabled" || status == "disabled", "statusText is enabled or disabled");
    check(controller.available() == (status == "enabled"),
          "available() agrees with the runtime status");
    const QString status_text = controller.statusText();
    if (controller.available()) {
        check(status_text == QStringLiteral("enabled"),
              "statusText is the bare runtime status when available");
    } else {
        check(status_text.contains(QStringLiteral("disabled")),
              "statusText names the disabled runtime");
        check(status_text.contains(QStringLiteral("GENESIS_AI_VISION=ON")),
              "statusText tells how to enable the runtime");
    }
}

void test_factor_clamps_to_steps()
{
    Fixture f = fixture();
    EnhanceController controller(&f.editor);

    controller.setFactor(0);
    check(controller.factor() == 1, "a zero ask snaps to 1");
    controller.setFactor(-5);
    check(controller.factor() == 1, "a negative ask snaps to 1");
    controller.setFactor(3);
    check(controller.factor() == 2, "an odd ask snaps down to 2");
    controller.setFactor(99);
    check(controller.factor() == 4, "a huge ask snaps to 4");
}

void test_disabled_refuses()
{
    Fixture f = fixture();
    EnhanceController controller(&f.editor);
    if (controller.available()) {
        return; // only the runtime-free build exercises this gate
    }
    controller.setClipId(QString::fromStdString(f.clip_id));

    bool finished = false;
    bool ok = true;
    QString error;
    QObject::connect(&controller, &EnhanceController::finished, [&](bool o, const QString &e) {
        finished = true;
        ok = o;
        error = e;
    });

    controller.applyEnhance();
    check(!controller.running(), "a disabled runtime does not start a run");
    check(controller.state() == QStringLiteral("Failed"), "the refusal reports Failed");
    check(!controller.error().isEmpty(), "the refusal leaves a message");
    check(error.contains(QStringLiteral("disabled")), "the refusal names the disabled runtime");
    check(finished && !ok, "finished reports the refusal");
}

void test_consent_declined()
{
    Fixture f = fixture();
    EnhanceController controller(&f.editor);
    if (!controller.available()) {
        return;
    }
    controller.setModelRoot(sandbox() / "decline");
    controller.setClipId(QString::fromStdString(f.clip_id));

    controller.applyEnhance();
    check(controller.awaitingConsent(), "a missing model parks in Consent");
    check(controller.state() == QStringLiteral("Consent"), "the state is Consent");
    check(controller.consentName() == QStringLiteral("real-esrgan 1.0"),
          "the consent prompt names the enhance model");
    check(!controller.consentSize().isEmpty(), "the consent prompt shows a size");
    check(controller.consentLicense() == QStringLiteral("BSD-3-Clause"),
          "the consent prompt shows the licence");

    controller.declineDownload();
    check(!controller.awaitingConsent(), "declining leaves Consent");
    check(controller.state() == QStringLiteral("Idle"), "declining returns to Idle");
    check(clip_media(f) != nullptr && clip_media(f)->id == f.media_id,
          "declining replaces nothing");
}

void test_success_applies_replace_and_single_undo()
{
    Fixture f = fixture();
    EnhanceController controller(&f.editor);
    if (!controller.available()) {
        return;
    }
    wire(controller, sandbox() / "success");
    controller.setClipId(QString::fromStdString(f.clip_id));

    bool finished_ok = false;
    QString finished_error;
    QObject::connect(&controller, &EnhanceController::finished, [&](bool ok, const QString &error) {
        finished_ok = ok;
        finished_error = error;
    });

    controller.applyEnhance();
    check(controller.awaitingConsent(), "the first run asks consent");
    controller.confirmDownload();
    check(controller.running(), "confirming starts the run");

    const bool done = wait_until_idle(controller, [&] { return !controller.running(); });
    check(done, "the run finishes within the timeout");
    check(controller.state() == QStringLiteral("Done"), "the run reports Done");
    check(controller.error().isEmpty(), "no error on success");
    check(controller.progress() >= 1.0, "progress reaches completion");
    check(finished_ok, "finished reports success");
    check(finished_error.isEmpty(), "a success has no error");

    // The run re-points the clip at a fresh video-only enhanced copy.
    const MediaItem *media = clip_media(f);
    check(media != nullptr && media->id != f.media_id, "the clip re-points at a new media item");
    if (media != nullptr) {
        check(media->origin.has_value() && *media->origin == MediaOrigin::Enhanced,
              "the copy is shelved under Enhanced");
        check(media->kind == MediaKind::Video, "the copy is a video");
        check(media->width.has_value() && *media->width == 16 && media->height.has_value()
                      && *media->height == 16,
              "the copy is 2x the 8x8 source");
        check(media->has_audio == false, "the copy drops audio");
        check(media->video_codec.has_value() && *media->video_codec == "vp8", "the copy is vp8");
        check(media->path.find("-enhanced.webm") != std::string::npos,
              "the copy is named <stem>-enhanced.webm");
    }

    // The whole run is one undo step, and undo points the clip back.
    check(f.editor.can_undo(), "the apply is a single undo step");
    f.editor.undo();
    check(clip_media(f) != nullptr && clip_media(f)->id == f.media_id,
          "undo re-points the clip at the original media");
}

void test_cancel_during_install()
{
    Fixture f = fixture();
    EnhanceController controller(&f.editor);
    if (!controller.available()) {
        return;
    }
    controller.setModelRoot(sandbox() / "cancel");
    controller.setOutputRoot(sandbox() / "cancel" / "out");
    controller.setClipId(QString::fromStdString(f.clip_id));

    // A fake installer that blocks until the test says stop, standing in for a
    // long download. On stop it reports failure; the worker sees the cancel
    // flag and reports Cancelled rather than Failed.
    std::atomic<bool> stop{ false };
    controller.setInstaller([&stop](const std::filesystem::path &, const ai::ModelEntry &,
                                    const ai::Fetcher &, const ai::Progress &) {
        while (!stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ai::InstallResult result;
        result.problems.push_back("download interrupted");
        return result;
    });

    controller.applyEnhance();
    controller.confirmDownload();
    check(controller.running(), "the run starts");

    QThread::msleep(20);
    controller.cancel();
    stop.store(true);

    const bool done = wait_until_idle(controller, [&] { return !controller.running(); });
    check(done, "the cancel lands within the timeout");
    check(controller.state() == QStringLiteral("Cancelled"), "the run reports itself cancelled");
    check(clip_media(f) != nullptr && clip_media(f)->id == f.media_id,
          "a cancelled run replaces nothing");
}

void test_enhance_error_surfaced()
{
    Fixture f = fixture();
    EnhanceController controller(&f.editor);
    if (!controller.available()) {
        return;
    }
    wire(controller, sandbox() / "enhance-err");
    controller.setClipId(QString::fromStdString(f.clip_id));
    controller.setEnhanceFn([](const vision::FrameView &, std::uint32_t, const vision::ProgressFn &,
                               const vision::AbortFn &) {
        vision::EnhanceOutcome out;
        out.error = "inference failed: bad tensor";
        return out;
    });

    bool finished_ok = true;
    QString finished_error;
    QObject::connect(&controller, &EnhanceController::finished, [&](bool ok, const QString &error) {
        finished_ok = ok;
        finished_error = error;
    });

    controller.applyEnhance();
    controller.confirmDownload();
    const bool done = wait_until_idle(controller, [&] { return !controller.running(); });
    check(done, "the run finishes within the timeout");
    check(controller.state() == QStringLiteral("Failed"), "an enhance failure reports Failed");
    check(controller.error() == QStringLiteral("inference failed: bad tensor"),
          "the enhance reason surfaces");
    check(!finished_ok && finished_error == QStringLiteral("inference failed: bad tensor"),
          "finished reports the failure");
    check(clip_media(f) != nullptr && clip_media(f)->id == f.media_id,
          "a failed run replaces nothing");
}

void test_frame_source_error_surfaced()
{
    Fixture f = fixture();
    EnhanceController controller(&f.editor);
    if (!controller.available()) {
        return;
    }
    wire(controller, sandbox() / "src-err");
    controller.setClipId(QString::fromStdString(f.clip_id));
    controller.setFrameSource([](const vision::FrameSink &) {
        vision::FrameSourceOutcome out;
        out.ok = false;
        out.error = "the file has no video stream";
        return out;
    });

    controller.applyEnhance();
    controller.confirmDownload();
    const bool done = wait_until_idle(controller, [&] { return !controller.running(); });
    check(done, "the run finishes within the timeout");
    check(controller.state() == QStringLiteral("Failed"), "a decode failure reports Failed");
    check(controller.error() == QStringLiteral("the file has no video stream"),
          "the decode reason surfaces");
    check(clip_media(f) != nullptr && clip_media(f)->id == f.media_id,
          "a failed run replaces nothing");
}

void test_encoder_error_surfaced()
{
    Fixture f = fixture();
    EnhanceController controller(&f.editor);
    if (!controller.available()) {
        return;
    }
    wire(controller, sandbox() / "enc-err");
    controller.setClipId(QString::fromStdString(f.clip_id));
    controller.setEncoder([](const EnhanceController::EncodeRequest &,
                             const EnhanceController::NextFrameFn &next,
                             const std::function<bool()> &cancel) {
        vision::FrameView frame;
        while (!cancel() && next(frame)) { }
        EnhanceController::EncodeOutcome out;
        out.error = "the encode pipeline rejected a frame";
        return out;
    });

    controller.applyEnhance();
    controller.confirmDownload();
    const bool done = wait_until_idle(controller, [&] { return !controller.running(); });
    check(done, "the run finishes within the timeout");
    check(controller.state() == QStringLiteral("Failed"), "an encode failure reports Failed");
    check(controller.error() == QStringLiteral("the encode pipeline rejected a frame"),
          "the encode reason surfaces");
    check(clip_media(f) != nullptr && clip_media(f)->id == f.media_id,
          "a failed run replaces nothing");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    test_status_and_availability();
    test_factor_clamps_to_steps();
    test_disabled_refuses();
    test_consent_declined();
    test_success_applies_replace_and_single_undo();
    test_cancel_during_install();
    test_enhance_error_surfaced();
    test_frame_source_error_surfaced();
    test_encoder_error_surfaced();

    std::filesystem::remove_all(sandbox());

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
