// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The studio shell's text-to-speech controller: the thin, testable layer that
// turns one "speak" request into a single undoable narration clip in the media
// bin and on the timeline. It owns no media and no engine - only the consent
// gate, the download/extract/synthesize worker, and the Batch of AddMedia +
// AddClipAtFirstFree applied through the project Editor. The QML sheet binds
// to the properties below and calls the Q_INVOKABLEs; the controller never
// touches QML.
//
// Flow. speak() refuses when the sherpa-onnx Kokoro runtime is not compiled in
// or the text is empty; when the pinned model's tarball is not yet unpacked it
// parks in Consent until confirmDownload() or declineDownload(); otherwise it
// starts one worker that (1) installs the tarball through ai::install, (2)
// unpacks it to a directory, (3) synthesizes the typed text to a WAV, and (4)
// hands the outcome back to the controller thread, where it becomes one Batch.
// Cancel flags the job and the atomic the worker polls; the run reports
// Cancelled when the worker next completes.
//
// Import. The synthesized WAV becomes NewMedia{origin=Speech, kind=Audio} and
// an AddClipAtFirstFree at the timeline start, wrapped in one Batch so the
// narration lands as a single undo step. The media id is minted by
// editor.peek_id("m") before the batch applies, so the AddClip command and the
// AddMedia command agree on the id without consuming a mint slot out of order.

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "ai/ModelStore.h"
#include "ai/tts/SherpaTtsProvider.h"
#include "app/controllers/AiRunController.h"

namespace genesis::project {
class Editor;
} // namespace genesis::project

class TtsController : public genesis::app::AiRunController
{
    Q_OBJECT

    // ---- Capability (static at runtime). ----

    // Whether the sherpa-onnx Kokoro runtime was compiled in. False in the
    // default build, where speak() refuses with a clear reason.
    Q_PROPERTY(bool available READ available CONSTANT)
    // "enabled" or "disabled": the runtime's own state, for the pane to show.
    Q_PROPERTY(QString statusText READ statusText CONSTANT)

    // ---- Inputs. ----

    // The text to speak.
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)
    // The speaker id from the Kokoro voice table (0 is the default voice).
    Q_PROPERTY(int voice READ voice WRITE setVoice NOTIFY voiceChanged)
    // The speaking rate; 1.0 is the voice's natural pace.
    Q_PROPERTY(double speed READ speed WRITE setSpeed NOTIFY speedChanged)

    // ---- The run's read-only view (the pane binds to these). ----

    // "Idle", "Consent", "Running", "Done", "Cancelled" or "Failed".
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    // True while a worker owns a run.
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    // How much of the whole run has finished, 0..1 (download then extract then
    // synthesis).
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    // The current stage: "downloading model", "extracting model",
    // "loading model", "synthesizing", "done".
    Q_PROPERTY(QString stage READ stage NOTIFY stageChanged)
    // The last problem, or the runtime's refusal; empty on success.
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)

    // ---- The consent gate (before the first download of a model). ----

    Q_PROPERTY(bool awaitingConsent READ awaitingConsent NOTIFY consentChanged)
    Q_PROPERTY(QString consentName READ consentName NOTIFY consentChanged)
    Q_PROPERTY(QString consentSize READ consentSize NOTIFY consentChanged)
    Q_PROPERTY(QString consentLicense READ consentLicense NOTIFY consentChanged)
    Q_PROPERTY(QString consentUrl READ consentUrl NOTIFY consentChanged)

    // ---- Voice selection (Kokoro speakers + cloned voices). ----

    // The number of speakers in the Kokoro voice table, 0 until a refresh has
    // loaded the model; the picker falls back to a single default voice.
    Q_PROPERTY(int speakerCount READ speakerCount NOTIFY speakerCountChanged)
    // The cloned voices the user has captured, by display name; each maps to a
    // <name>.wav under the voice root.
    Q_PROPERTY(QStringList clonedVoices READ clonedVoices NOTIFY clonedVoicesChanged)
    // The currently selected cloned voice (empty selects the Kokoro voices).
    Q_PROPERTY(QString clonedVoice READ clonedVoice NOTIFY clonedVoiceChanged)

public:
    // A seam for model installation: given the root, the entry, the transport
    // and the progress callback, returns the installed tarball path. Tests
    // inject a fake that hands back a canned path - the pinned Kokoro model's
    // 132 MB of real bytes (and its SHA-256) cannot be fabricated in a unit
    // test. The default is the real ai::install.
    using Installer = std::function<genesis::ai::InstallResult(
            const std::filesystem::path &, const genesis::ai::ModelEntry &,
            const genesis::ai::Fetcher &, const genesis::ai::Progress &)>;

    // A seam for unpacking the tarball into a directory. Tests inject a fake
    // that reports a clean unpack; the default shells out to `tar` because the
    // provider stays engine- and shell-free and the tarball is a multi-file
    // package the single-file ModelStore does not unpack.
    using Extractor = std::function<genesis::ai::tts::ExtractResult(
            const std::filesystem::path &tarball, const std::filesystem::path &dest_dir)>;

    // A seam for synthesis: takes the model directory, the model kind and the
    // text, returns the written WAV. Tests inject a fake; the default reaches
    // the real SherpaTtsProvider.
    using Synthesizer = std::function<genesis::ai::tts::SynthesizeOutcome(
            std::string_view model_dir, genesis::ai::tts::ModelKind kind, std::string_view text,
            const genesis::ai::tts::SynthesizeOptions &, std::string_view wav_path,
            const genesis::ai::tts::ProgressFn &, const genesis::ai::tts::AbortFn &)>;

    // A seam for counting the Kokoro model's speakers. Tests inject a fake
    // (loading the ONNX graph is expensive); the default reaches the real
    // SherpaTtsProvider::num_speakers.
    using SpeakerCounter = std::function<std::int32_t(std::string_view model_dir)>;

    explicit TtsController(genesis::project::Editor *editor, QObject *parent = nullptr);
    ~TtsController() override;

    bool available() const override { return genesis::ai::tts::SherpaTtsProvider::available(); }
    // The runtime's own state, but human-readable: the provider reports a bare
    // "enabled"/"disabled", so a disabled runtime is expanded to the same
    // reason speak() refuses with, rather than the bare word the pane would
    // otherwise show.
    QString statusText() const override
    {
        if (!available()) {
            return QStringLiteral("disabled: sherpa-onnx TTS runtime not compiled in "
                                  "(rebuild with GENESIS_AI_TTS=ON)");
        }
        const std::string_view status = genesis::ai::tts::SherpaTtsProvider::status();
        return QString::fromUtf8(status.data(), static_cast<qsizetype>(status.size()));
    }

    QString text() const { return text_; }
    void setText(const QString &text);
    int voice() const { return voice_; }
    void setVoice(int voice);
    double speed() const { return speed_; }
    void setSpeed(double speed);

    int speakerCount() const { return speakerCount_; }
    QStringList clonedVoices() const { return clonedVoices_; }
    QString clonedVoice() const { return clonedVoice_; }

    // ---- Injection points (tests swap these in before a run). ----

    void setModelRoot(const std::filesystem::path &root) { model_root_ = root; }
    void setOutputRoot(const std::filesystem::path &root) { output_root_ = root; }
    void setInstaller(Installer installer) { installer_ = std::move(installer); }
    void setExtractor(Extractor extractor) { extractor_ = std::move(extractor); }
    void setSynthesizer(Synthesizer synthesizer) { synthesizer_ = std::move(synthesizer); }
    void setSpeakerCounter(SpeakerCounter counter) { speakerCounter_ = std::move(counter); }
    // The directory cloned voice recordings live in (<name>.wav per voice).
    // Setting it also scans for existing recordings, so a restart re-lists the
    // voices the user already captured.
    void setVoiceRoot(const std::filesystem::path &root);

    // Starts a narration run for the typed text: refuses when the runtime is
    // disabled or the text is empty, parks in Consent when the model must first
    // be downloaded, and otherwise runs install -> extract -> synthesize off
    // the UI thread. No-ops while a run is already in flight.
    Q_INVOKABLE void speak();

    // Loads the Kokoro model off-thread and reports its speaker count, so the
    // picker can list the real voices rather than a single default. No-ops
    // while a refresh is already in flight; a missing or not-yet-downloaded
    // model reports 0.
    Q_INVOKABLE void refreshSpeakers();

    // Captures the WAV at `source_url` (a file:// URL from the picker) as a new
    // cloned voice: copies it into the voice root under a unique name, lists it
    // and selects it. Refuses (Failed + reason) when the runtime is disabled,
    // the source is unreadable or the copy fails.
    Q_INVOKABLE void cloneVoice(const QString &source_url);

    // Forgets a cloned voice: deletes its recording and clears the selection
    // when it was the selected voice. No-ops for an unknown name.
    Q_INVOKABLE void removeClonedVoice(const QString &name);

    // Selects a cloned voice (empty name returns to the Kokoro voices). No-ops
    // when the selection is unchanged.
    Q_INVOKABLE void selectClonedVoice(const QString &name);

signals:
    void textChanged();
    void voiceChanged();
    void speedChanged();
    void speakerCountChanged();
    void clonedVoicesChanged();
    void clonedVoiceChanged();

private:
    void startRun() override;
    bool modelPresent() const override;
    const char *discardLog() const override
    {
        return "[app] speech discarded: the project changed mid-run\n";
    }

    // Whether the model is ready to synthesize: the tarball is installed and
    // its unpack directory already holds the model files for `kind`. A cheap
    // existence check; the authoritative digest verify runs inside ai::install
    // on the worker.
    bool modelReady(const genesis::ai::ModelEntry &entry, genesis::ai::tts::ModelKind kind) const;

    // Posts finishSpeech onto the controller's thread, copying the worker's
    // results so the worker (which is returning) can drop them.
    void postFinish(bool ok, const std::string &error, const std::string &wav_path, double duration,
                    std::uint32_t sample_rate);

    // Runs on the controller's thread when the worker's outcome lands: folds
    // the outcome into the scheduler and the properties, applies the batch,
    // emits the signals, then joins the worker.
    void finishSpeech(bool ok, const QString &error, const std::string &wav_path, double duration,
                      std::uint32_t sample_rate);

    // Turns the synthesized WAV into one Batch of AddMedia + AddClipAtFirstFree
    // and applies it. False when the editor refused it (the reason lands in
    // `refusal`).
    bool applySpeech(const std::string &wav_path, double duration, std::uint32_t sample_rate,
                     QString &refusal);

    QString text_;
    int voice_ = 0;
    double speed_ = 1.0;

    // Voice selection state: the Kokoro speaker count (loaded lazily off the UI
    // thread) and the captured cloned voices plus the current selection.
    int speakerCount_ = 0;
    QStringList clonedVoices_;
    QString clonedVoice_;

    // The pinned Kokoro entry, resolved from the manifest once at construction
    // so the consent prompt and the worker agree on the model.
    std::optional<genesis::ai::ModelEntry> model_entry_;
    // The pinned PocketTts entry (the zero-shot voice-cloning model), resolved
    // alongside Kokoro; used only when a cloned voice is selected.
    std::optional<genesis::ai::ModelEntry> pocket_entry_;

    // The resolved run's parameters, fixed at speak() (the worker and the
    // apply both read them; the text and tunables are the ones speak() saw).
    genesis::ai::ModelEntry runEntry_;
    genesis::ai::tts::ModelKind runKind_ = genesis::ai::tts::ModelKind::Kokoro;
    std::string runReferenceWav_;
    QString runText_;
    int runVoice_ = 0;
    double runSpeed_ = 1.0;
    std::filesystem::path runWavPath_;

    std::filesystem::path model_root_;
    std::filesystem::path output_root_;
    std::filesystem::path voice_root_;
    Installer installer_;
    Extractor extractor_;
    Synthesizer synthesizer_;
    SpeakerCounter speakerCounter_;

    // A monotonically increasing suffix for the WAV name, so consecutive runs
    // within a session never collide ("speech-1.wav", "speech-2.wav", ...).
    int speech_counter_ = 0;

    // The speaker-count loader's thread: refreshSpeakers() loads the Kokoro
    // graph off the UI thread and posts the count back. Joined in the
    // destructor alongside the base's workerThread_.
    std::thread speakersThread_;
};