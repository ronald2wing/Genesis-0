// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The studio shell's AI captions controller: the thin, testable layer that
// turns one "captions" request into a single undoable batch of title clips.
// It owns no media and no engine - only the consent gate, the download/extract/
// transcribe worker, and the mapping from a transcript's segments to a Batch of
// AddTextClip commands applied through the project Editor. The QML pane binds
// to the properties below and calls the Q_INVOKABLEs; the controller never
// touches QML.
//
// Flow. generateCaptions() refuses when the whisper runtime is not compiled in
// or the selected clip has no audio; when the pinned model is not yet on disk
// it parks in Consent until confirmDownload() or declineDownload(); otherwise
// it starts one worker that (1) installs the model through ai::install (with
// the consent it implied), (2) extracts the clip's audio to 16 kHz mono f32
// PCM, (3) transcribes it, and (4) hands the segments back to the controller
// thread, where they become one Batch. Cancel flags the job and the atomic the
// worker polls; the run reports Cancelled when the worker next completes.
//
// Mapping. A segment [start, end) becomes one AddTextClip with
// start = clip.start + segment.start, duration = end - start, above = true and
// a default TextStyle carrying the segment's text. Segments are relative to the
// media start (the extractor decodes the whole stream); source_start and speed
// correction are deliberately out of scope for v1, so a caption sits over the
// clip's media-time, not its trimmed window.

#pragma once

#include <QObject>
#include <QString>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "ai/ModelStore.h"
#include "ai/captions/WhisperProvider.h"
#include "adapters/engine/ges/media/AudioFrames.h"
#include "app/controllers/AiRunController.h"
#include "core/ClipTimeline.h"
#include "jobs/Job.h"

namespace genesis::project {
class Editor;
struct Clip;
} // namespace genesis::project

class AiController : public genesis::app::AiRunController
{
    Q_OBJECT

    // ---- Capability (static at runtime). ----

    // Whether the whisper.cpp caption runtime was compiled in. False in the
    // default build, where generateCaptions refuses with a clear reason.
    Q_PROPERTY(bool available READ available CONSTANT)
    // "enabled" or "disabled": the runtime's own state, for the pane to show.
    Q_PROPERTY(QString statusText READ statusText CONSTANT)

    // ---- Target. ----

    // The clip being captioned. Set by the shell, which already owns selection;
    // empty means nothing is selected and generateCaptions refuses.
    Q_PROPERTY(QString clipId READ clipId WRITE setClipId NOTIFY clipIdChanged)

    // ---- The run's read-only view (the pane binds to these). ----

    // "Idle", "Consent", "Running", "Done", "Cancelled" or "Failed".
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    // True while a worker owns a run.
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    // How much of the whole run has finished, 0..1 (download then extraction
    // then transcription).
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    // The current stage: "downloading model", "extracting audio",
    // "loading model", "transcribing", "done".
    Q_PROPERTY(QString stage READ stage NOTIFY stageChanged)
    // The last problem, or the engine's refusal; empty on success.
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)

    // ---- The consent gate (before the first download of a model). ----

    Q_PROPERTY(bool awaitingConsent READ awaitingConsent NOTIFY consentChanged)
    Q_PROPERTY(QString consentName READ consentName NOTIFY consentChanged)
    Q_PROPERTY(QString consentSize READ consentSize NOTIFY consentChanged)
    Q_PROPERTY(QString consentLicense READ consentLicense NOTIFY consentChanged)
    Q_PROPERTY(QString consentUrl READ consentUrl NOTIFY consentChanged)

public:
    // A seam for transcription: takes the installed model path and a raw PCM
    // path, returns the outcome. Tests inject a fake; the default reaches the
    // real WhisperProvider.
    using Transcriber = std::function<genesis::ai::TranscribeOutcome(
            std::string_view model_path, std::string_view pcm_path,
            const genesis::ai::TranscribeOptions &, const genesis::ai::ProgressFn &,
            const genesis::ai::AbortFn &)>;

    // A seam for audio extraction: turns a request into its result. Tests
    // inject a fake so no GStreamer decode runs; the default is the real
    // ges::extract_audio.
    using Extractor = std::function<genesis::adapters::engine::ges::AudioExtractResult(
            const genesis::adapters::engine::ges::AudioExtractRequest &)>;

    // A seam for model installation: given the root, the entry, the transport
    // and the progress callback, returns the installed model path. Tests inject
    // a fake that hands back a canned path - the pinned whisper model's real
    // bytes (and its SHA-256) cannot be fabricated in a unit test, and the
    // controller must stay testable without a 147 MB download. The default is
    // the real ai::install.
    using Installer = std::function<genesis::ai::InstallResult(
            const std::filesystem::path &, const genesis::ai::ModelEntry &,
            const genesis::ai::Fetcher &, const genesis::ai::Progress &)>;

    explicit AiController(genesis::project::Editor *editor, QObject *parent = nullptr);

    bool available() const override { return genesis::ai::WhisperProvider::available(); }
    // The runtime's own state, but human-readable: the provider reports a bare
    // "enabled"/"disabled", so a disabled runtime is expanded to the same
    // reason generateCaptions() refuses with.
    QString statusText() const override
    {
        if (!available()) {
            return QStringLiteral("disabled: whisper.cpp runtime not compiled in "
                                  "(rebuild with GENESIS_AI_CAPTIONS=ON)");
        }
        const std::string_view status = genesis::ai::WhisperProvider::status();
        return QString::fromUtf8(status.data(), static_cast<qsizetype>(status.size()));
    }

    QString clipId() const { return clipId_; }
    void setClipId(const QString &clipId);

    // ---- Injection points (tests swap these in before a run). ----

    void setFetcher(genesis::ai::Fetcher fetcher) { fetcher_ = std::move(fetcher); }
    void setTranscriber(Transcriber transcriber) { transcriber_ = std::move(transcriber); }
    void setExtractor(Extractor extractor) { extractor_ = std::move(extractor); }
    void setInstaller(Installer installer) { installer_ = std::move(installer); }
    // Where installed models live (<root>/<id>/<version>/<file>). The app sets
    // it to AppDataLocation/models; a test sets a scratch dir.
    void setModelRoot(const std::filesystem::path &root) { model_root_ = root; }

    // Starts a caption run over the selected clip: refuses when the runtime is
    // disabled or the clip has no audio, parks in Consent when the model must
    // first be downloaded, and otherwise runs install -> extract -> transcribe
    // off the UI thread. No-ops while a run is already in flight.
    Q_INVOKABLE void generateCaptions();

signals:
    void clipIdChanged();

private:
    // The selected clip on the active timeline, or null when none/unknown.
    const genesis::project::Clip *clip() const;

    // Starts the worker for the already-resolved run (members runEntry_,
    // runClipStart_, runMediaPath_, runStreamIndex_).
    void startRun() override;

    // Posts finishCaptions onto the controller's thread, copying the worker's
    // results so the worker (which is returning) can drop them.
    void postFinish(bool ok, const std::string &error,
                    const std::vector<genesis::jobs::CaptionSegment> &segments,
                    const genesis::core::Rational &clipStart);

    // Runs on the controller's thread when the worker's outcome lands: folds
    // the outcome into the scheduler and the properties, applies the batch,
    // emits the signals, then joins the worker.
    void finishCaptions(bool ok, const QString &error,
                        const std::vector<genesis::jobs::CaptionSegment> &segments,
                        const genesis::core::Rational &clipStart);

    // Turns the segments into one Batch of AddTextClip and applies it. False
    // when the editor refused it (the reason lands in `refusal`).
    bool applyCaptions(const std::vector<genesis::jobs::CaptionSegment> &segments,
                       const genesis::core::Rational &clipStart, QString &refusal);

    // Whether the resolved model file exists on disk (a cheap existence check;
    // the authoritative digest verify runs inside ai::install on the worker).
    bool modelPresent() const override;

    const char *discardLog() const override
    {
        return "[app] captions discarded: the project changed mid-run\n";
    }

    QString clipId_;

    // The resolved run's parameters, fixed at generateCaptions (the worker and
    // the apply both read them; a clip moved mid-run places captions at the
    // position it had when the run began - see the header note).
    genesis::ai::ModelEntry runEntry_;
    genesis::core::Rational runClipStart_ = genesis::core::Rational::ZERO;
    std::string runMediaPath_;
    std::uint32_t runStreamIndex_ = 0;

    std::filesystem::path model_root_;
    genesis::ai::Fetcher fetcher_;
    Transcriber transcriber_;
    Extractor extractor_;
    Installer installer_;
};