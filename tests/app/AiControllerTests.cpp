// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The AI captions controller's suite: drives consent -> install -> extract ->
// transcribe -> apply headlessly with injected fakes, so no model download, no
// GStreamer decode and no whisper.cpp run happens in CI. The pinned whisper
// model's 147 MB of real bytes cannot be fabricated, so the installer seam
// stands in for the model store; the extractor and transcriber seams stand in
// for the engine and the runtime.

#include <atomic>
#include <chrono>
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
#include "app/controllers/AiController.h"
#include "jobs/Job.h"
#include "project/Editor.h"
#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/model/Media.h"
#include "project/model/Text.h"

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
namespace ges = genesis::adapters::engine::ges;
namespace jobs = genesis::jobs;
using genesis::core::Rational;
using genesis::test::wait_until_idle;
using namespace genesis::project;

// A project with one media item and one clip on the first track. `has_audio`
// chooses whether generateCaptions gets past its audio gate.
struct Fixture
{
    Editor editor;
    std::string media_id;
    std::string clip_id;
};

Fixture fixture(bool has_audio, const Rational &clip_start = Rational{ 2, 1 })
{
    Fixture f;
    NewMedia item;
    item.path = "/footage/a.webm";
    item.name = "a.webm";
    item.duration = Rational{ 10, 1 };
    item.kind = MediaKind::Video;
    item.width = 640;
    item.height = 360;
    item.has_audio = has_audio;
    if (has_audio) {
        AudioTrack track;
        track.index = 0;
        item.audio_tracks.push_back(track);
    }
    const auto added = f.editor.apply(Command{ AddMedia{ std::move(item) } });
    check(added.has_value() && added->created_id.has_value(), "adds media");
    f.media_id = *added->created_id;
    const std::string track_id = f.editor.project().timelines[0].tracks[0].id;
    const auto placed =
            f.editor.apply(Command{ AddClip{ f.media_id, track_id, clip_start, false } });
    check(placed.has_value() && placed->created_id.has_value(), "adds a clip");
    f.clip_id = *placed->created_id;
    return f;
}

// Points the controller's seams at fakes: a model root in a scratch dir (so the
// consent gate fires), an installer that hands back a canned path without a
// download, and an extractor that reports a clean decode. The caller then sets
// the transcriber for the scenario under test.
void wire(AiController &controller, const std::filesystem::path &root)
{
    controller.setModelRoot(root);
    controller.setInstaller([](const std::filesystem::path &, const ai::ModelEntry &,
                               const ai::Fetcher &, const ai::Progress &) {
        ai::InstallResult result;
        result.installed = std::filesystem::path("/fake/ggml-base.en.bin");
        return result;
    });
    controller.setExtractor([](const ges::AudioExtractRequest &) {
        ges::AudioExtractResult result;
        result.error = ges::AudioExtractError::None;
        result.samples = 16000;
        result.duration_seconds = 1.0;
        return result;
    });
}

// A per-run sandbox under the system temp dir, emptied first so a crashed run
// cannot leak a fresh-looking model file into the assertions.
const std::filesystem::path &sandbox()
{
    static const std::filesystem::path dir = [] {
        const std::filesystem::path p =
                std::filesystem::temp_directory_path() / "genesis-ai-controller-tests";
        std::filesystem::remove_all(p);
        std::filesystem::create_directories(p);
        return p;
    }();
    return dir;
}

// The title clips the run placed, in insertion order (the source clip, being
// video, is excluded).
std::vector<const Clip *> text_clips(const Editor &editor)
{
    std::vector<const Clip *> out;
    for (const Clip &clip : editor.project().active().clips) {
        if (clip.kind == ClipKind::Text) {
            out.push_back(&clip);
        }
    }
    return out;
}

void test_status_and_availability()
{
    Fixture f = fixture(true);
    AiController controller(&f.editor);

    const std::string status{ ai::WhisperProvider::status() };
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
        check(status_text.contains(QStringLiteral("GENESIS_AI_CAPTIONS=ON")),
              "statusText tells how to enable the runtime");
    }
}

void test_disabled_refuses()
{
    Fixture f = fixture(true);
    AiController controller(&f.editor);
    if (controller.available()) {
        return; // only the runtime-free build exercises this gate
    }
    controller.setClipId(QString::fromStdString(f.clip_id));

    bool finished = false;
    bool ok = true;
    QString error;
    QObject::connect(&controller, &AiController::finished, [&](bool o, const QString &e) {
        finished = true;
        ok = o;
        error = e;
    });

    controller.generateCaptions();
    check(!controller.running(), "a disabled runtime does not start a run");
    check(controller.state() == QStringLiteral("Failed"), "the refusal reports Failed");
    check(!controller.error().isEmpty(), "the refusal leaves a message");
    check(error.contains(QStringLiteral("disabled")), "the refusal names the disabled runtime");
    check(finished && !ok, "finished reports the refusal");
}

void test_consent_declined()
{
    Fixture f = fixture(true);
    AiController controller(&f.editor);
    if (!controller.available()) {
        return;
    }
    controller.setModelRoot(sandbox() / "decline");
    controller.setClipId(QString::fromStdString(f.clip_id));

    controller.generateCaptions();
    check(controller.awaitingConsent(), "a missing model parks in Consent");
    check(controller.state() == QStringLiteral("Consent"), "the state is Consent");
    check(controller.consentName() == QStringLiteral("whisper base.en"),
          "the consent prompt names the model");
    check(!controller.consentSize().isEmpty(), "the consent prompt shows a size");
    check(controller.consentLicense() == QStringLiteral("MIT"),
          "the consent prompt shows the licence");

    controller.declineDownload();
    check(!controller.awaitingConsent(), "declining leaves Consent");
    check(controller.state() == QStringLiteral("Idle"), "declining returns to Idle");
    check(text_clips(f.editor).empty(), "declining places no captions");
}

void test_segment_mapping_and_single_undo()
{
    Fixture f = fixture(true, Rational{ 2, 1 });
    AiController controller(&f.editor);
    if (!controller.available()) {
        return;
    }
    wire(controller, sandbox() / "map");
    controller.setClipId(QString::fromStdString(f.clip_id));

    // Two valid segments, one empty-text and one degenerate span to prove the
    // run skips them.
    std::vector<jobs::CaptionSegment> segments;
    segments.push_back({ 0.5, 1.5, "hello" });
    segments.push_back({ 2.0, 2.5, "world" });
    segments.push_back({ 3.0, 4.0, "" });
    segments.push_back({ 4.5, 4.0, "backwards" });
    controller.setTranscriber([segments](std::string_view, std::string_view,
                                         const ai::TranscribeOptions &, const ai::ProgressFn &,
                                         const ai::AbortFn &) {
        ai::TranscribeOutcome outcome;
        outcome.ok = true;
        outcome.transcript.segments = segments;
        return outcome;
    });

    bool finished_ok = false;
    QString finished_error;
    QObject::connect(&controller, &AiController::finished, [&](bool ok, const QString &error) {
        finished_ok = ok;
        finished_error = error;
    });

    controller.generateCaptions();
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

    const std::vector<const Clip *> placed = text_clips(f.editor);
    check(placed.size() == 2, "two valid segments become two captions");
    if (placed.size() == 2) {
        // The source clip sits at 2s; segment start 0.5 -> timeline 2.5s, one
        // second long.
        const Clip *hello = placed[0];
        check(hello->start == Rational{ 5, 2 }, "the first caption sits at 2.5s");
        check(hello->duration == Rational{ 1, 1 }, "the first caption runs one second");
        check(hello->text.has_value() && hello->text->content == "hello",
              "the first caption carries its text");

        const Clip *world = placed[1];
        check(world->start == Rational{ 4, 1 }, "the second caption sits at 4s");
        check(world->duration == Rational{ 1, 2 }, "the second caption runs half a second");
        check(world->text.has_value() && world->text->content == "world",
              "the second caption carries its text");
    }

    // The whole run is one undo step, and undo takes every caption back at
    // once.
    check(f.editor.can_undo(), "the batch is a single undo step");
    const std::size_t before = f.editor.project().active().clips.size();
    f.editor.undo();
    check(text_clips(f.editor).empty(), "undo removes every caption at once");
    check(f.editor.project().active().clips.size() == before - 2,
          "undo restores the pre-run clip count");
}

void test_silent_transcript_no_edit()
{
    Fixture f = fixture(true);
    AiController controller(&f.editor);
    if (!controller.available()) {
        return;
    }
    wire(controller, sandbox() / "silent");
    controller.setClipId(QString::fromStdString(f.clip_id));
    controller.setTranscriber([](std::string_view, std::string_view, const ai::TranscribeOptions &,
                                 const ai::ProgressFn &, const ai::AbortFn &) {
        ai::TranscribeOutcome outcome;
        outcome.ok = true; // no segments
        return outcome;
    });

    controller.generateCaptions();
    controller.confirmDownload();
    const bool done = wait_until_idle(controller, [&] { return !controller.running(); });
    check(done, "the run finishes within the timeout");
    check(controller.state() == QStringLiteral("Done"), "a silent transcript still reports Done");
    check(text_clips(f.editor).empty(), "no segments means no clips");
    // The run recorded nothing: undoing once unwinds the fixture's own
    // AddClip (back to zero clips), not a caption batch stacked on top. The
    // fixture already recorded two edits, so `can_undo()` is true either way.
    f.editor.undo();
    check(f.editor.project().active().clips.empty(),
          "no edit was recorded: one undo removes the fixture's clip");
}

void test_no_audio_refuses()
{
    Fixture f = fixture(false);
    AiController controller(&f.editor);
    if (!controller.available()) {
        return;
    }
    controller.setClipId(QString::fromStdString(f.clip_id));
    controller.generateCaptions();
    check(controller.state() == QStringLiteral("Failed"), "a clip without audio refuses");
    check(controller.error() == QStringLiteral("The selected clip has no audio."),
          "the refusal names the missing audio");
}

void test_cancel_during_install()
{
    Fixture f = fixture(true);
    AiController controller(&f.editor);
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

    controller.generateCaptions();
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
    check(text_clips(f.editor).empty(), "a cancelled run places nothing");
}

void test_transcribe_error_surfaced()
{
    Fixture f = fixture(true);
    AiController controller(&f.editor);
    if (!controller.available()) {
        return;
    }
    wire(controller, sandbox() / "error");
    controller.setClipId(QString::fromStdString(f.clip_id));
    controller.setTranscriber([](std::string_view, std::string_view, const ai::TranscribeOptions &,
                                 const ai::ProgressFn &, const ai::AbortFn &) {
        ai::TranscribeOutcome outcome;
        outcome.ok = false;
        outcome.error = "model failed to load";
        return outcome;
    });

    bool finished_ok = true;
    QString finished_error;
    QObject::connect(&controller, &AiController::finished, [&](bool ok, const QString &error) {
        finished_ok = ok;
        finished_error = error;
    });

    controller.generateCaptions();
    controller.confirmDownload();
    const bool done = wait_until_idle(controller, [&] { return !controller.running(); });
    check(done, "the run finishes within the timeout");
    check(controller.state() == QStringLiteral("Failed"), "a transcribe failure reports Failed");
    check(controller.error() == QStringLiteral("model failed to load"),
          "the failure reason surfaces");
    check(!finished_ok && finished_error == QStringLiteral("model failed to load"),
          "finished reports the failure");
    check(text_clips(f.editor).empty(), "a failed run places nothing");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    test_status_and_availability();
    test_disabled_refuses();
    test_consent_declined();
    test_segment_mapping_and_single_undo();
    test_silent_transcript_no_edit();
    test_no_audio_refuses();
    test_cancel_during_install();
    test_transcribe_error_surfaced();

    std::filesystem::remove_all(sandbox());

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
