// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The AI cutout controller's suite: drives consent -> install -> decode ->
// segment -> apply headlessly with injected fakes, so no model download, no
// GStreamer decode and no ONNX run happens in CI. The pinned vision models'
// real bytes cannot be fabricated, so the installer seam stands in for the
// model store; the frame source and segment seams stand in for the engine and
// the runtime. The MaskDriver itself runs for real (it is engine- and
// runtime-free), writing synthetic masks to a scratch root.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <QCoreApplication>
#include <QString>
#include <QThread>

#include "../support/Wait.h"

#include "ai/ModelStore.h"
#include "ai/vision/MaskDriver.h"
#include "app/controllers/CutoutController.h"
#include "jobs/Job.h"
#include "project/Editor.h"
#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/command/PropertyCommands.h"
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

// A project with one video media item and one clip on the first track.
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
    item.width = 640;
    item.height = 360;
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

// The selected clip's cutout, or null for a clip that has none.
const std::optional<Cutout> *cutout_of(const Fixture &f)
{
    const Clip *clip = f.editor.project().active().clip(f.clip_id);
    return clip == nullptr ? nullptr : &clip->cutout;
}

// Pumps `count` synthetic 2x2 RGB frames into the driver's sink. The frames'
// bytes are never read - the fake segment ignores them - so a real decode is
// stood in for without any engine.
CutoutController::FrameSource pump_frames(int count)
{
    return [count](const vision::FrameSink &sink) {
        std::array<std::uint8_t, 12> rgb{ };
        for (int i = 0; i < count; ++i) {
            const vision::FrameView frame{ rgb.data(), 2, 2 };
            if (!sink(frame, static_cast<std::uint32_t>(i))) {
                break;
            }
        }
        vision::FrameSourceOutcome out;
        out.ok = true;
        return out;
    };
}

// A segment seam that returns a half-transparent mask for every frame, so the
// real MaskDriver stores synthetic masks without a model or a runtime.
CutoutController::SegmentFn canned_alpha()
{
    return [](const vision::FrameView &frame, const vision::ProgressFn &, const vision::AbortFn &) {
        vision::SegmentationOutcome out;
        out.ok = true;
        out.width = frame.width;
        out.height = frame.height;
        out.alpha.assign(static_cast<std::size_t>(frame.width) * frame.height, 0.5f);
        return out;
    };
}

// Points the controller's seams at fakes: a model root in a scratch dir (so the
// consent gate fires), a mask root beside it, an installer that hands back a
// canned path without a download, two synthetic frames, and a fake segmenter.
// The caller then overrides the frame source or segmenter for the scenario
// under test.
void wire(CutoutController &controller, const std::filesystem::path &root)
{
    controller.setModelRoot(root);
    controller.setMaskRoot(root / "masks");
    controller.setInstaller([](const std::filesystem::path &, const ai::ModelEntry &,
                               const ai::Fetcher &, const ai::Progress &) {
        ai::InstallResult result;
        result.installed = std::filesystem::path("/fake/model.onnx");
        return result;
    });
    controller.setFrameSource(pump_frames(2));
    controller.setSegmentFn(canned_alpha());
}

// A per-run sandbox under the system temp dir, emptied first so a crashed run
// cannot leak a fresh-looking model file into the assertions.
const std::filesystem::path &sandbox()
{
    static const std::filesystem::path dir = [] {
        const std::filesystem::path p =
                std::filesystem::temp_directory_path() / "genesis-cutout-controller-tests";
        std::filesystem::remove_all(p);
        std::filesystem::create_directories(p);
        return p;
    }();
    return dir;
}

void test_status_and_availability()
{
    Fixture f = fixture();
    CutoutController controller(&f.editor);

    const std::string status{ vision::SegmentationProvider::status() };
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

void test_disabled_refuses()
{
    Fixture f = fixture();
    CutoutController controller(&f.editor);
    if (controller.available()) {
        return; // only the runtime-free build exercises this gate
    }
    controller.setClipId(QString::fromStdString(f.clip_id));

    bool finished = false;
    bool ok = true;
    QString error;
    QObject::connect(&controller, &CutoutController::finished, [&](bool o, const QString &e) {
        finished = true;
        ok = o;
        error = e;
    });

    controller.applyCutout(QStringLiteral("person"));
    check(!controller.running(), "a disabled runtime does not start a run");
    check(controller.state() == QStringLiteral("Failed"), "the refusal reports Failed");
    check(!controller.error().isEmpty(), "the refusal leaves a message");
    check(error.contains(QStringLiteral("disabled")), "the refusal names the disabled runtime");
    check(finished && !ok, "finished reports the refusal");
}

void test_unknown_subject_refuses()
{
    Fixture f = fixture();
    CutoutController controller(&f.editor);
    if (!controller.available()) {
        return; // the disabled refusal fires first in the default build
    }
    controller.setClipId(QString::fromStdString(f.clip_id));
    controller.applyCutout(QStringLiteral("auto"));
    check(controller.state() == QStringLiteral("Failed"), "an unmodelled subject refuses");
    check(controller.error() == QStringLiteral("That subject has no cutout model."),
          "the refusal names the missing model");
    check(cutout_of(f) == nullptr || !cutout_of(f)->has_value(), "the refusal places no cutout");
}

void test_consent_declined()
{
    Fixture f = fixture();
    CutoutController controller(&f.editor);
    if (!controller.available()) {
        return;
    }
    controller.setModelRoot(sandbox() / "decline");
    controller.setClipId(QString::fromStdString(f.clip_id));

    controller.applyCutout(QStringLiteral("person"));
    check(controller.awaitingConsent(), "a missing model parks in Consent");
    check(controller.state() == QStringLiteral("Consent"), "the state is Consent");
    check(controller.consentName() == QStringLiteral("rvm 1.0"),
          "the consent prompt names the person model");
    check(!controller.consentSize().isEmpty(), "the consent prompt shows a size");
    check(controller.consentLicense() == QStringLiteral("GPL-3.0"),
          "the consent prompt shows the licence");

    controller.declineDownload();
    check(!controller.awaitingConsent(), "declining leaves Consent");
    check(controller.state() == QStringLiteral("Idle"), "declining returns to Idle");
    check(!cutout_of(f)->has_value(), "declining places no cutout");
}

void test_success_applies_person_cutout_and_single_undo()
{
    Fixture f = fixture();
    CutoutController controller(&f.editor);
    if (!controller.available()) {
        return;
    }
    wire(controller, sandbox() / "person");
    controller.setClipId(QString::fromStdString(f.clip_id));

    bool finished_ok = false;
    QString finished_error;
    QObject::connect(&controller, &CutoutController::finished, [&](bool ok, const QString &error) {
        finished_ok = ok;
        finished_error = error;
    });

    controller.applyCutout(QStringLiteral("person"));
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

    // The run applies one automatic person cutout.
    const std::optional<Cutout> *cutout = cutout_of(f);
    check(cutout != nullptr && cutout->has_value(), "the clip gains a cutout");
    if (cutout != nullptr && cutout->has_value()) {
        check(cutout->value().mode == CutoutMode::Auto, "the cutout is automatic");
        check(cutout->value().subject == Subject::Person, "the cutout keeps the person");
    }

    // The whole run is one undo step, and undo takes the cutout back.
    check(f.editor.can_undo(), "the apply is a single undo step");
    f.editor.undo();
    check(!cutout_of(f)->has_value(), "undo removes the cutout");
}

void test_object_maps_to_isnet()
{
    Fixture f = fixture();
    CutoutController controller(&f.editor);
    if (!controller.available()) {
        return;
    }
    wire(controller, sandbox() / "object");
    controller.setClipId(QString::fromStdString(f.clip_id));

    controller.applyCutout(QStringLiteral("object"));
    check(controller.awaitingConsent(), "a missing model parks in Consent");
    check(controller.consentName() == QStringLiteral("isnet 1.0"),
          "the object subject names the IS-Net model");
    check(controller.consentLicense() == QStringLiteral("Apache-2.0"),
          "the IS-Net model shows its licence");
    controller.confirmDownload();

    const bool done = wait_until_idle(controller, [&] { return !controller.running(); });
    check(done, "the run finishes within the timeout");
    check(controller.state() == QStringLiteral("Done"), "the run reports Done");

    const std::optional<Cutout> *cutout = cutout_of(f);
    check(cutout != nullptr && cutout->has_value() && cutout->value().subject == Subject::Object,
          "the object run applies an object cutout");
}

void test_cancel_during_install()
{
    Fixture f = fixture();
    CutoutController controller(&f.editor);
    if (!controller.available()) {
        return;
    }
    controller.setModelRoot(sandbox() / "cancel");
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

    controller.applyCutout(QStringLiteral("person"));
    controller.confirmDownload();
    check(controller.running(), "the run starts");

    // Give the worker a moment to enter the installer, then cancel and let it
    // unblock.
    QThread::msleep(20);
    controller.cancel();
    stop.store(true);

    const bool done = wait_until_idle(controller, [&] { return !controller.running(); });
    check(done, "the cancel lands within the timeout");
    check(controller.state() == QStringLiteral("Cancelled"), "the run reports itself cancelled");
    check(!cutout_of(f)->has_value(), "a cancelled run places nothing");
}

void test_segment_error_surfaced()
{
    Fixture f = fixture();
    CutoutController controller(&f.editor);
    if (!controller.available()) {
        return;
    }
    wire(controller, sandbox() / "segerr");
    controller.setClipId(QString::fromStdString(f.clip_id));
    controller.setSegmentFn(
            [](const vision::FrameView &, const vision::ProgressFn &, const vision::AbortFn &) {
                vision::SegmentationOutcome out;
                out.error = "inference failed: bad tensor";
                return out;
            });

    bool finished_ok = true;
    QString finished_error;
    QObject::connect(&controller, &CutoutController::finished, [&](bool ok, const QString &error) {
        finished_ok = ok;
        finished_error = error;
    });

    controller.applyCutout(QStringLiteral("person"));
    controller.confirmDownload();
    const bool done = wait_until_idle(controller, [&] { return !controller.running(); });
    check(done, "the run finishes within the timeout");
    check(controller.state() == QStringLiteral("Failed"), "a segmentation failure reports Failed");
    check(controller.error() == QStringLiteral("inference failed: bad tensor"),
          "the segmentation reason surfaces");
    check(!finished_ok && finished_error == QStringLiteral("inference failed: bad tensor"),
          "finished reports the failure");
    check(!cutout_of(f)->has_value(), "a failed run places nothing");
}

void test_frame_source_error_surfaced()
{
    Fixture f = fixture();
    CutoutController controller(&f.editor);
    if (!controller.available()) {
        return;
    }
    wire(controller, sandbox() / "srcerr");
    controller.setClipId(QString::fromStdString(f.clip_id));
    controller.setFrameSource([](const vision::FrameSink &) {
        vision::FrameSourceOutcome out;
        out.ok = false;
        out.error = "the file has no video stream";
        return out;
    });

    controller.applyCutout(QStringLiteral("object"));
    controller.confirmDownload();
    const bool done = wait_until_idle(controller, [&] { return !controller.running(); });
    check(done, "the run finishes within the timeout");
    check(controller.state() == QStringLiteral("Failed"), "a decode failure reports Failed");
    check(controller.error() == QStringLiteral("the file has no video stream"),
          "the decode reason surfaces");
    check(!cutout_of(f)->has_value(), "a failed run places nothing");
}

void test_clear_cutout()
{
    // clearCutout is model-free, so it runs in both builds.
    Fixture f = fixture();
    CutoutController controller(&f.editor);
    controller.setClipId(QString::fromStdString(f.clip_id));

    // Seed a cutout through the editor so clear has something to remove.
    SetClipCutout seed;
    seed.clip_id = f.clip_id;
    seed.cutout = Cutout{ CutoutMode::Auto, Subject::Person, DEFAULT_FEATHER, { } };
    check(f.editor.apply(Command{ std::move(seed) }).has_value(), "the seed cutout applies");
    check(cutout_of(f)->has_value(), "the clip has a cutout before clearing");

    controller.clearCutout();
    check(!cutout_of(f)->has_value(), "clearing removes the cutout");

    // clearCutout is one undo step: undoing restores the seed cutout.
    f.editor.undo();
    check(cutout_of(f)->has_value(), "undo restores the cleared cutout");
}

// Applies a custom cutout with one smart stroke at instant 0 to the fixture's
// clip, so a regenerate has strokes and a revision to work from. Returns true
// when the seed applied.
bool seed_smart_stroke_cutout(Fixture &f, Subject subject)
{
    SetClipCutout seed;
    seed.clip_id = f.clip_id;
    Cutout cutout;
    cutout.mode = CutoutMode::Custom;
    cutout.subject = subject;
    cutout.feather = DEFAULT_FEATHER;
    Stroke stroke;
    stroke.tool = BrushTool::SmartBrush;
    stroke.size = 0.05;
    stroke.points = { { 0.1, 0.2 }, { 0.3, 0.4 } };
    stroke.at = Rational{ 0, 1 };
    cutout.strokes.push_back(std::move(stroke));
    seed.cutout = std::move(cutout);
    return f.editor.apply(Command{ std::move(seed) }).has_value();
}

void test_regenerate_refuses_without_cutout()
{
    // Structural refusals fire before the runtime gate, so this runs in both
    // builds.
    Fixture f = fixture();
    CutoutController controller(&f.editor);
    controller.setClipId(QString::fromStdString(f.clip_id));

    bool finished = false;
    QObject::connect(&controller, &CutoutController::finished,
                     [&](bool, const QString &) { finished = true; });

    controller.regenerateClip(QString::fromStdString(f.clip_id));
    check(controller.state() == QStringLiteral("Failed"), "a clip with no cutout refuses");
    check(controller.error() == QStringLiteral("The clip has no cutout to regenerate."),
          "the refusal names the missing cutout");
    check(finished, "finished reports the refusal");
}

void test_regenerate_refuses_auto_subject()
{
    Fixture f = fixture();
    CutoutController controller(&f.editor);
    controller.setClipId(QString::fromStdString(f.clip_id));

    // A custom cutout painted before a subject was chosen: Auto names no
    // model, so there is nothing to regenerate against.
    check(seed_smart_stroke_cutout(f, Subject::Auto), "the Auto-subject cutout seeds");
    controller.regenerateClip(QString::fromStdString(f.clip_id));
    check(controller.state() == QStringLiteral("Failed"),
          "an Auto-subject cutout refuses to regenerate");
    check(controller.error().contains(QStringLiteral("Apply a cutout subject")),
          "the refusal asks for a subject first");
}

void test_regenerate_refines_strokes_and_adds_no_undo()
{
    Fixture f = fixture();
    CutoutController controller(&f.editor);
    if (!controller.available()) {
        return; // the runtime gate fires before a regenerate can run
    }
    wire(controller, sandbox() / "regenerate");
    controller.setClipId(QString::fromStdString(f.clip_id));
    check(seed_smart_stroke_cutout(f, Subject::Person), "the Person cutout seeds");

    // A fake refine seam records the strokes the driver hands it, standing in
    // for the SlimSAM forward pass.
    std::vector<vision::BrushStroke> refined;
    controller.setBrushRefineFn(
            [&refined](std::vector<float> &alpha, const vision::FrameView &frame,
                       const std::vector<vision::BrushStroke> &strokes, std::string &error) {
                refined = strokes;
                (void)alpha;
                (void)frame;
                (void)error;
                return true;
            });

    int history_changes = 0;
    int project_edits = 0;
    bool finished_ok = false;
    QObject::connect(&controller, &CutoutController::historyChanged, [&]() { ++history_changes; });
    QObject::connect(&controller, &CutoutController::projectEdited, [&]() { ++project_edits; });
    QObject::connect(&controller, &CutoutController::finished,
                     [&](bool ok, const QString &) { finished_ok = ok; });

    controller.regenerateClip(QString::fromStdString(f.clip_id));
    check(controller.awaitingConsent(), "a regenerate with models absent parks in Consent");
    controller.confirmDownload();

    const bool done = wait_until_idle(controller, [&] { return !controller.running(); });
    check(done, "the regenerate finishes within the timeout");
    check(controller.state() == QStringLiteral("Done"), "the regenerate reports Done");
    check(finished_ok, "finished reports success");

    // The smart stroke at instant 0 maps to frame 0, which the two synthetic
    // frames deliver, so the refine seam sees exactly that stroke.
    check(refined.size() == 1, "the refine seam sees the smart stroke");
    if (!refined.empty()) {
        check(refined[0].tool == BrushTool::SmartBrush, "the refined stroke is the smart brush");
        check(refined[0].frame == 0, "the stroke names frame 0");
    }

    // A regenerate re-keys and refines masks already on the clip: no command,
    // so no undo step and no historyChanged - only the project-edited signal.
    check(history_changes == 0, "a regenerate adds no undo step");
    check(project_edits >= 1, "a regenerate signals projectEdited");

    // The seeded cutout is still on the clip (regenerate did not touch it), and
    // one undo reaches the pre-seed state, proving no extra command was pushed.
    check(cutout_of(f) != nullptr && cutout_of(f)->has_value(), "the cutout stays on the clip");
    f.editor.undo();
    check(!cutout_of(f)->has_value(), "one undo reaches the pre-seed clip");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    test_status_and_availability();
    test_disabled_refuses();
    test_unknown_subject_refuses();
    test_consent_declined();
    test_success_applies_person_cutout_and_single_undo();
    test_object_maps_to_isnet();
    test_cancel_during_install();
    test_segment_error_surfaced();
    test_frame_source_error_surfaced();
    test_clear_cutout();
    test_regenerate_refuses_without_cutout();
    test_regenerate_refuses_auto_subject();
    test_regenerate_refines_strokes_and_adds_no_undo();

    std::filesystem::remove_all(sandbox());

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
