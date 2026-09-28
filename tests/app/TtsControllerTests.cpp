// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The text-to-speech controller's suite: drives consent -> install -> extract
// -> synthesize -> apply headlessly with injected fakes, so no model download,
// no tar extraction and no sherpa-onnx run happens in CI. The pinned Kokoro
// model's 132 MB of real bytes cannot be fabricated, so the installer seam
// stands in for the model store; the extractor and synthesizer seams stand in
// for the host tar and the runtime.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>

#include <QCoreApplication>
#include <QString>
#include <QThread>

#include "../support/Checks.h"
#include "../support/Wait.h"

#include "ai/ModelStore.h"
#include "ai/tts/SherpaTtsProvider.h"
#include "app/controllers/TtsController.h"
#include "project/Editor.h"
#include "project/command/Command.h"
#include "project/model/Clip.h"
#include "project/model/Media.h"

namespace {

namespace ai = genesis::ai;
using genesis::core::Rational;
using genesis::test::check;
using genesis::test::pause;
using genesis::test::summary;
using genesis::test::wait_until;
using genesis::test::wait_until_idle;
using namespace genesis::project;

// Points the controller's seams at fakes: a model root in a scratch dir (so the
// consent gate fires), an installer that hands back a canned path without a
// download, and an extractor that reports a clean unpack. The caller then sets
// the synthesizer for the scenario under test.
void wire(TtsController &controller, const std::filesystem::path &root)
{
    controller.setModelRoot(root);
    controller.setOutputRoot(root);
    controller.setInstaller([](const std::filesystem::path &, const ai::ModelEntry &,
                               const ai::Fetcher &, const ai::Progress &) {
        ai::InstallResult result;
        result.installed = std::filesystem::path("/fake/kokoro.tar.bz2");
        return result;
    });
    controller.setExtractor([](const std::filesystem::path &, const std::filesystem::path &dest) {
        ai::tts::ExtractResult result;
        result.ok = true;
        result.dir = dest;
        return result;
    });
}

// A per-run sandbox under the system temp dir, emptied first so a crashed run
// cannot leak a fresh-looking model file into the assertions.
const std::filesystem::path &sandbox()
{
    static const std::filesystem::path dir = [] {
        const std::filesystem::path p =
                std::filesystem::temp_directory_path() / "genesis-tts-controller-tests";
        std::filesystem::remove_all(p);
        std::filesystem::create_directories(p);
        return p;
    }();
    return dir;
}

// A canned successful synthesis: writes nothing, but reports the run's WAV
// path, a two-second duration and a 24 kHz sample rate.
ai::tts::SynthesizeOutcome fake_synthesize(std::string_view wav_path)
{
    ai::tts::SynthesizeOutcome outcome;
    outcome.ok = true;
    outcome.wav_path = std::string(wav_path);
    outcome.duration = 2.0;
    outcome.sample_rate = 24000;
    return outcome;
}

void test_status_and_availability()
{
    Editor editor;
    TtsController controller(&editor);

    const std::string status{ ai::tts::SherpaTtsProvider::status() };
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
        check(status_text.contains(QStringLiteral("GENESIS_AI_TTS=ON")),
              "statusText tells how to enable the runtime");
    }
}

void test_disabled_refuses()
{
    Editor editor;
    TtsController controller(&editor);
    if (controller.available()) {
        return; // only the runtime-free build exercises this gate
    }
    controller.setText(QStringLiteral("hello"));

    bool finished = false;
    bool ok = true;
    QString error;
    QObject::connect(&controller, &TtsController::finished, [&](bool o, const QString &e) {
        finished = true;
        ok = o;
        error = e;
    });

    controller.speak();
    check(!controller.running(), "a disabled runtime does not start a run");
    check(controller.state() == QStringLiteral("Failed"), "the refusal reports Failed");
    check(!controller.error().isEmpty(), "the refusal leaves a message");
    check(error.contains(QStringLiteral("disabled")), "the refusal names the disabled runtime");
    check(finished && !ok, "finished reports the refusal");
}

void test_empty_text_refuses()
{
    Editor editor;
    TtsController controller(&editor);
    if (!controller.available()) {
        return;
    }
    controller.speak();
    check(controller.state() == QStringLiteral("Failed"), "empty text refuses");
    check(controller.error() == QStringLiteral("Type something to speak first."),
          "the refusal asks for text");
}

void test_consent_declined()
{
    Editor editor;
    TtsController controller(&editor);
    if (!controller.available()) {
        return;
    }
    controller.setModelRoot(sandbox() / "decline");
    controller.setText(QStringLiteral("hello"));

    controller.speak();
    check(controller.awaitingConsent(), "a missing model parks in Consent");
    check(controller.state() == QStringLiteral("Consent"), "the state is Consent");
    check(controller.consentName() == QStringLiteral("kokoro int8-multi-lang-v1_0"),
          "the consent prompt names the model");
    check(!controller.consentSize().isEmpty(), "the consent prompt shows a size");
    check(controller.consentLicense() == QStringLiteral("Apache-2.0"),
          "the consent prompt shows the licence");

    controller.declineDownload();
    check(!controller.awaitingConsent(), "declining leaves Consent");
    check(controller.state() == QStringLiteral("Idle"), "declining returns to Idle");
    check(editor.project().media.empty(), "declining places no media");
}

void test_success_applies_batch_in_one_undo()
{
    Editor editor;
    TtsController controller(&editor);
    if (!controller.available()) {
        return;
    }
    wire(controller, sandbox() / "success");
    controller.setText(QStringLiteral("hello world"));
    controller.setSynthesizer([](std::string_view, ai::tts::ModelKind, std::string_view,
                                 const ai::tts::SynthesizeOptions &, std::string_view wav_path,
                                 const ai::tts::ProgressFn &,
                                 const ai::tts::AbortFn &) { return fake_synthesize(wav_path); });

    bool finished_ok = false;
    QString finished_error;
    QObject::connect(&controller, &TtsController::finished, [&](bool ok, const QString &error) {
        finished_ok = ok;
        finished_error = error;
    });

    controller.speak();
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

    // The narration lands as one generated audio media item and one audio clip
    // at the timeline start, in a single undo step.
    check(editor.project().media.size() == 1, "one media item was added");
    check(editor.project().active().clips.size() == 1, "one clip was added");
    if (editor.project().media.size() == 1) {
        const MediaItem &item = editor.project().media[0];
        check(item.kind == MediaKind::Audio, "the narration is audio media");
        check(item.has_audio, "the narration carries an audio stream");
        check(item.origin.has_value() && *item.origin == MediaOrigin::Speech,
              "the narration is shelved under Speech");
        check(item.audio_tracks.size() == 1 && item.audio_tracks[0].sample_rate == 24000,
              "the narration's sample rate is recorded");
    }
    if (editor.project().active().clips.size() == 1) {
        const Clip &clip = editor.project().active().clips[0];
        check(clip.kind == ClipKind::Audio, "the narration clip is audio");
        check(clip.start == Rational::ZERO, "the clip sits at the start");
        check(clip.duration == Rational{ 2, 1 }, "the clip runs the synthesized two seconds");
        check(editor.project().media.size() == 1 && clip.media_id == editor.project().media[0].id,
              "the clip references the narration media");
    }

    // The whole import is one undo step, and undo takes both back at once.
    check(editor.can_undo(), "the batch is a single undo step");
    editor.undo();
    check(editor.project().media.empty(), "undo removes the media");
    check(editor.project().active().clips.empty(), "undo removes the clip");
}

void test_cancel_during_install()
{
    Editor editor;
    TtsController controller(&editor);
    if (!controller.available()) {
        return;
    }
    controller.setModelRoot(sandbox() / "cancel");
    controller.setText(QStringLiteral("hello"));

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

    controller.speak();
    controller.confirmDownload();
    check(controller.running(), "the run starts");

    // Give the worker a moment to enter the installer, then cancel and let it
    // unblock.
    pause(20);
    controller.cancel();
    stop.store(true);

    const bool done = wait_until_idle(controller, [&] { return !controller.running(); });
    check(done, "the cancel lands within the timeout");
    check(controller.state() == QStringLiteral("Cancelled"), "the run reports itself cancelled");
    check(editor.project().media.empty(), "a cancelled run places nothing");
}

void test_project_change_discards()
{
    Editor editor;
    TtsController controller(&editor);
    if (!controller.available()) {
        return;
    }
    wire(controller, sandbox() / "swap");
    controller.setText(QStringLiteral("hello"));

    // A blocking synthesizer that eventually reports success: without the epoch
    // guard the narration would land on whatever project the editor holds when
    // the run finishes.
    std::atomic<bool> stop{ false };
    controller.setSynthesizer([&stop](std::string_view, ai::tts::ModelKind, std::string_view,
                                      const ai::tts::SynthesizeOptions &, std::string_view wav_path,
                                      const ai::tts::ProgressFn &, const ai::tts::AbortFn &) {
        while (!stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return fake_synthesize(wav_path);
    });

    bool finished_ok = true;
    QString finished_error;
    QObject::connect(&controller, &TtsController::finished, [&](bool ok, const QString &error) {
        finished_ok = ok;
        finished_error = error;
    });

    controller.speak();
    controller.confirmDownload();
    check(controller.running(), "the run starts");

    pause(20);
    editor.load(Project::with_video(VideoSettings{ }));
    stop.store(true);

    const bool done = wait_until_idle(controller, [&] { return !controller.running(); });
    check(done, "the discard lands within the timeout");
    check(controller.state() == QStringLiteral("Cancelled"), "a stale run reports Cancelled");
    check(controller.error() == QStringLiteral("project changed"), "the discard names the cause");
    check(!finished_ok && finished_error == QStringLiteral("project changed"),
          "finished reports the discard");
    check(editor.project().media.empty(), "no narration lands on the new project");
}

void test_installer_error_surfaced()
{
    Editor editor;
    TtsController controller(&editor);
    if (!controller.available()) {
        return;
    }
    controller.setModelRoot(sandbox() / "install-error");
    controller.setText(QStringLiteral("hello"));
    controller.setInstaller([](const std::filesystem::path &, const ai::ModelEntry &,
                               const ai::Fetcher &, const ai::Progress &) {
        ai::InstallResult result;
        result.problems.push_back("the model download failed");
        return result;
    });

    bool finished_ok = true;
    QString finished_error;
    QObject::connect(&controller, &TtsController::finished, [&](bool ok, const QString &error) {
        finished_ok = ok;
        finished_error = error;
    });

    controller.speak();
    controller.confirmDownload();
    const bool done = wait_until_idle(controller, [&] { return !controller.running(); });
    check(done, "the run finishes within the timeout");
    check(controller.state() == QStringLiteral("Failed"), "an install failure reports Failed");
    check(controller.error() == QStringLiteral("the model download failed"),
          "the failure reason surfaces");
    check(!finished_ok && finished_error == QStringLiteral("the model download failed"),
          "finished reports the failure");
    check(editor.project().media.empty(), "a failed run places nothing");
}

void test_extractor_error_surfaced()
{
    Editor editor;
    TtsController controller(&editor);
    if (!controller.available()) {
        return;
    }
    wire(controller, sandbox() / "extract-error");
    controller.setText(QStringLiteral("hello"));
    controller.setExtractor([](const std::filesystem::path &, const std::filesystem::path &) {
        ai::tts::ExtractResult result;
        result.error = "the model archive could not be unpacked";
        return result;
    });

    controller.speak();
    controller.confirmDownload();
    const bool done = wait_until_idle(controller, [&] { return !controller.running(); });
    check(done, "the run finishes within the timeout");
    check(controller.state() == QStringLiteral("Failed"), "an extract failure reports Failed");
    check(controller.error() == QStringLiteral("the model archive could not be unpacked"),
          "the failure reason surfaces");
    check(editor.project().media.empty(), "a failed run places nothing");
}

void test_synthesizer_error_surfaced()
{
    Editor editor;
    TtsController controller(&editor);
    if (!controller.available()) {
        return;
    }
    wire(controller, sandbox() / "synthesize-error");
    controller.setText(QStringLiteral("hello"));
    controller.setSynthesizer([](std::string_view, ai::tts::ModelKind, std::string_view,
                                 const ai::tts::SynthesizeOptions &, std::string_view,
                                 const ai::tts::ProgressFn &, const ai::tts::AbortFn &) {
        ai::tts::SynthesizeOutcome outcome;
        outcome.error = "model failed to load";
        return outcome;
    });

    bool finished_ok = true;
    QString finished_error;
    QObject::connect(&controller, &TtsController::finished, [&](bool ok, const QString &error) {
        finished_ok = ok;
        finished_error = error;
    });

    controller.speak();
    controller.confirmDownload();
    const bool done = wait_until_idle(controller, [&] { return !controller.running(); });
    check(done, "the run finishes within the timeout");
    check(controller.state() == QStringLiteral("Failed"), "a synthesis failure reports Failed");
    check(controller.error() == QStringLiteral("model failed to load"),
          "the failure reason surfaces");
    check(!finished_ok && finished_error == QStringLiteral("model failed to load"),
          "finished reports the failure");
    check(editor.project().media.empty(), "a failed run places nothing");
}

// Writes a small file (dummy bytes), creating parent directories, so a test can
// plant a reference recording or a model file without a real payload.
std::filesystem::path plant_file(const std::filesystem::path &path)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    out << "RIFF0000WAVE";
    return path;
}

void test_clone_voice()
{
    Editor editor;
    TtsController controller(&editor);
    if (!controller.available()) {
        return;
    }
    const std::filesystem::path root = sandbox() / "clone";
    const std::filesystem::path source = plant_file(root / "reference.wav");
    const std::filesystem::path voices = root / "voices";
    controller.setVoiceRoot(voices);

    controller.cloneVoice(QString::fromStdString(source.string()));

    check(controller.clonedVoices().size() == 1, "one voice is captured");
    check(controller.clonedVoices().contains(QStringLiteral("reference")),
          "the voice is named after the source stem");
    check(controller.clonedVoice() == QStringLiteral("reference"), "the cloned voice is selected");
    check(std::filesystem::is_regular_file(voices / "reference.wav"),
          "the recording is copied into the voice root");
    check(controller.state() != QStringLiteral("Failed"), "cloning does not fail");
}

void test_clone_voice_refuses_unreadable()
{
    Editor editor;
    TtsController controller(&editor);
    if (!controller.available()) {
        return;
    }
    const std::filesystem::path voices = sandbox() / "clone-refuse";
    controller.setVoiceRoot(voices);

    controller.cloneVoice(QStringLiteral("/no/such/recording.wav"));

    check(controller.state() == QStringLiteral("Failed"), "an unreadable source refuses");
    check(controller.error() == QStringLiteral("Choose a WAV recording to clone."),
          "the refusal asks for a WAV");
    check(controller.clonedVoices().isEmpty(), "nothing was captured");
}

void test_remove_cloned_voice()
{
    Editor editor;
    TtsController controller(&editor);
    if (!controller.available()) {
        return;
    }
    const std::filesystem::path root = sandbox() / "remove";
    const std::filesystem::path source = plant_file(root / "alice.wav");
    const std::filesystem::path voices = root / "voices";
    controller.setVoiceRoot(voices);
    controller.cloneVoice(QString::fromStdString(source.string()));

    const std::filesystem::path copied = voices / "alice.wav";
    controller.removeClonedVoice(QStringLiteral("alice"));

    check(controller.clonedVoices().isEmpty(), "the voice is forgotten");
    check(controller.clonedVoice().isEmpty(), "the selection is cleared");
    check(!std::filesystem::exists(copied), "the recording is deleted");
}

void test_select_cloned_voice()
{
    Editor editor;
    TtsController controller(&editor);
    if (!controller.available()) {
        return;
    }
    const std::filesystem::path root = sandbox() / "select";
    const std::filesystem::path voices = root / "voices";
    plant_file(voices / "alice.wav");
    plant_file(voices / "bob.wav");
    controller.setVoiceRoot(voices); // lists alice + bob

    controller.selectClonedVoice(QStringLiteral("bob"));
    check(controller.clonedVoice() == QStringLiteral("bob"),
          "selecting a cloned voice changes the selection");

    controller.selectClonedVoice(QString());
    check(controller.clonedVoice().isEmpty(), "an empty name returns to the Kokoro voices");
}

void test_refresh_speakers()
{
    Editor editor;
    TtsController controller(&editor);
    if (!controller.available()) {
        return;
    }
    const std::filesystem::path root = sandbox() / "speakers";
    controller.setModelRoot(root);
    // The Kokoro model file the loader's cheap readiness check looks for.
    plant_file(root / "kokoro" / "int8-multi-lang-v1_0" / "unpacked" / "model.int8.onnx");
    controller.setSpeakerCounter([](std::string_view) { return 5; });

    controller.refreshSpeakers();

    check(wait_until([&] { return controller.speakerCount() == 5; }),
          "the speaker count lands from the loader");
}

void test_pocket_tts_path()
{
    Editor editor;
    TtsController controller(&editor);
    if (!controller.available()) {
        return;
    }
    wire(controller, sandbox() / "pocket");
    const std::filesystem::path voices = sandbox() / "pocket" / "voices";
    plant_file(voices / "alice.wav");
    controller.setVoiceRoot(voices);
    controller.selectClonedVoice(QStringLiteral("alice"));

    bool saw_pocket = false;
    std::string reference_wav;
    controller.setSynthesizer([&](std::string_view, ai::tts::ModelKind kind, std::string_view,
                                  const ai::tts::SynthesizeOptions &options,
                                  std::string_view wav_path, const ai::tts::ProgressFn &,
                                  const ai::tts::AbortFn &) {
        saw_pocket = (kind == ai::tts::ModelKind::PocketTts);
        reference_wav = options.reference_wav;
        return fake_synthesize(wav_path);
    });

    controller.setText(QStringLiteral("hello"));
    controller.speak();
    check(controller.awaitingConsent(), "the cloning run asks consent");
    check(controller.consentName() == QStringLiteral("pocket-tts int8-2026-01-26"),
          "the consent prompt names the cloning model");

    controller.confirmDownload();
    const bool done = wait_until_idle(controller, [&] { return !controller.running(); });
    check(done, "the run finishes within the timeout");
    check(controller.state() == QStringLiteral("Done"), "the cloning run reports Done");
    check(saw_pocket, "the synthesizer saw the PocketTts kind");
    check(reference_wav == (voices / "alice.wav").string(),
          "the reference recording is forwarded to the synthesizer");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    test_status_and_availability();
    test_disabled_refuses();
    test_empty_text_refuses();
    test_consent_declined();
    test_success_applies_batch_in_one_undo();
    test_cancel_during_install();
    test_project_change_discards();
    test_installer_error_surfaced();
    test_extractor_error_surfaced();
    test_synthesizer_error_surfaced();
    test_clone_voice();
    test_clone_voice_refuses_unreadable();
    test_remove_cloned_voice();
    test_select_cloned_voice();
    test_refresh_speakers();
    test_pocket_tts_path();

    std::filesystem::remove_all(sandbox());

    return summary();
}
