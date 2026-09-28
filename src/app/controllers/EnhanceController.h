// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The studio shell's AI enhance controller: the thin, testable layer that turns
// one "enhance" request into a single undoable ReplaceClipMedia over the
// selected clip. It owns no media and no engine - only the consent gate, the
// download/decode/enhance/encode worker, and the mapping from the run's output
// to the command that re-points the clip at it. The QML pane binds to the
// properties below and calls the Q_INVOKABLEs; the controller never touches
// QML. Its public surface (state, progress, error, consent) mirrors
// AiController's and CutoutController's exactly, so the UI can share one dialog
// shape for all three.
//
// Flow. applyEnhance() refuses when the ONNX enhance runtime is not compiled in
// or no clip is selected; when the pinned Real-ESRGAN model is not yet on disk
// it parks in Consent until confirmDownload() or declineDownload(). Otherwise it
// starts one worker that (1) installs the model through ai::install, (2) loads
// the EnhanceProvider, (3) decodes the clip's video to RGB frames and enhances
// each to the effective factor, (4) encodes the enhanced frames through the
// injected Encoder seam (the default is a provisional GStreamer VP8/WebM
// pipeline - the engine-side encode seam is owned by a parallel scope), and (5)
// hands the outcome back to the controller thread, where it becomes one
// ReplaceClipMedia pointing the clip at the new video-only copy. Cancel flags
// the job and the atomic the worker polls; the run reports Cancelled when the
// worker next completes.
//
// Mapping. The enhanced file is a video-only copy (audio is dropped, exactly
// like a reversed copy - ReplaceClipMedia documents that shape), written next to
// the source as "<stem>-enhanced.webm", described by a NewMedia whose origin is
// MediaOrigin::Enhanced so the bin shelves it under Generated.

#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "ai/ModelStore.h"
#include "ai/vision/EnhanceProvider.h"
#include "ai/vision/MaskDriver.h"
#include "jobs/Job.h"
#include "jobs/Scheduler.h"
#include "project/command/MediaCommands.h"
#include "project/model/Media.h"

namespace genesis::project {
class Editor;
struct Clip;
} // namespace genesis::project

class EnhanceController : public QObject
{
    Q_OBJECT

    // ---- Capability (static at runtime). ----

    // Whether the ONNX enhance runtime was compiled in. False in the default
    // build, where applyEnhance refuses with a clear reason.
    Q_PROPERTY(bool available READ available CONSTANT)
    // "enabled" or "disabled": the runtime's own state, for the pane to show.
    Q_PROPERTY(QString statusText READ statusText CONSTANT)

    // ---- Target. ----

    // The clip being enhanced. Set by the shell, which already owns selection;
    // empty means nothing is selected and applyEnhance refuses.
    Q_PROPERTY(QString clipId READ clipId WRITE setClipId NOTIFY clipIdChanged)

    // ---- The run's shape. ----

    // How many times bigger the enhanced copy is: 1 (denoise only), 2 or 4.
    // The effective factor may step down when even the ask would exceed the
    // 4096-pixel longest-side cap.
    Q_PROPERTY(int factor READ factor WRITE setFactor NOTIFY factorChanged)

    // ---- The run's read-only view (the pane binds to these). ----

    // "Idle", "Consent", "Running", "Done", "Cancelled" or "Failed".
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    // True while a worker owns a run.
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    // How much of the whole run has finished, 0..1 (download then enhance).
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    // The current stage: "downloading model", "enhancing", "done".
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
    // A seam for model installation: given the root, the entry, the transport
    // and the progress callback, returns the installed model path. Tests inject
    // a fake that hands back a canned path - the pinned model's real bytes (and
    // its SHA-256) cannot be fabricated in a unit test. The default is the real
    // ai::install.
    using Installer = std::function<genesis::ai::InstallResult(
            const std::filesystem::path &, const genesis::ai::ModelEntry &,
            const genesis::ai::Fetcher &, const genesis::ai::Progress &)>;

    // A seam for frame decoding: pumps decoded RGB frames into the producer.
    // Tests inject a fake so no GStreamer decode runs; the default wraps
    // ges::extract_video_frames.
    using FrameSource = genesis::ai::vision::FrameSource;

    // A seam for enhancement: one frame and a factor in, one enhanced frame
    // out. Tests inject a fake returning canned pixels so no model and no
    // runtime load; the default loads the EnhanceProvider once and reuses its
    // session per frame.
    using EnhanceFn = std::function<genesis::ai::vision::EnhanceOutcome(
            const genesis::ai::vision::FrameView &, std::uint32_t factor,
            const genesis::ai::vision::ProgressFn &, const genesis::ai::vision::AbortFn &)>;

    // ---- The encode seam (the engine-side counterpart to the decode seam). ----

    struct EncodeOutcome
    {
        bool ok = false;
        std::string error;
    };

    struct EncodeRequest
    {
        // The file the encoder writes.
        std::filesystem::path destination;
        // The fixed enhanced-frame size (even, so VP8's 4:2:0 accepts it).
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        // The output frame rate as an exact fraction.
        std::uint32_t fps_num = 30;
        std::uint32_t fps_den = 1;
    };

    // Pulls the next enhanced frame into `frame` (the rgb pointer only; width
    // and height are fixed by EncodeRequest). Returns false when the frames are
    // exhausted.
    using NextFrameFn = std::function<bool(genesis::ai::vision::FrameView &frame)>;

    // Encodes the enhanced frames into `destination` and returns the outcome.
    // The default is a provisional GStreamer VP8/WebM pipeline; tests inject a
    // fake so no GStreamer encode runs.
    using Encoder = std::function<EncodeOutcome(const EncodeRequest &, const NextFrameFn &,
                                                const std::function<bool()> &)>;

    explicit EnhanceController(genesis::project::Editor *editor, QObject *parent = nullptr);
    ~EnhanceController() override;

    bool available() const { return genesis::ai::vision::EnhanceProvider::available(); }
    // The runtime's own state, but human-readable: the provider reports a bare
    // "enabled"/"disabled", so a disabled runtime is expanded to the same
    // reason applyEnhance() refuses with.
    QString statusText() const
    {
        if (!available()) {
            return QStringLiteral("disabled: ONNX Runtime not compiled in "
                                  "(rebuild with GENESIS_AI_VISION=ON)");
        }
        const std::string_view status = genesis::ai::vision::EnhanceProvider::status();
        return QString::fromUtf8(status.data(), static_cast<qsizetype>(status.size()));
    }

    QString clipId() const { return clipId_; }
    void setClipId(const QString &clipId);

    int factor() const { return factor_; }
    void setFactor(int factor);

    QString state() const { return state_; }
    bool running() const { return running_; }
    double progress() const { return progress_; }
    QString stage() const { return stage_; }
    QString error() const { return error_; }

    bool awaitingConsent() const { return state_ == QStringLiteral("Consent"); }
    QString consentName() const { return consentName_; }
    QString consentSize() const { return consentSize_; }
    QString consentLicense() const { return consentLicense_; }
    QString consentUrl() const { return consentUrl_; }

    // ---- Injection points (tests swap these in before a run). ----

    void setFetcher(genesis::ai::Fetcher fetcher) { fetcher_ = std::move(fetcher); }
    void setInstaller(Installer installer) { installer_ = std::move(installer); }
    void setFrameSource(FrameSource source) { frameSource_ = std::move(source); }
    void setEnhanceFn(EnhanceFn enhance) { enhanceFn_ = std::move(enhance); }
    void setEncoder(Encoder encoder) { encoder_ = std::move(encoder); }
    // Where installed models live (<root>/<id>/<version>/<file>). The app sets
    // it to AppDataLocation/models; a test sets a scratch dir.
    void setModelRoot(const std::filesystem::path &root) { model_root_ = root; }
    // Where the enhanced copies land. The app sets it next to the media the
    // app opened; a test sets a scratch dir.
    void setOutputRoot(const std::filesystem::path &root) { output_root_ = root; }

    // Starts an enhance run over the selected clip: refuses when the runtime is
    // disabled or the clip has no video, parks in Consent when the model must
    // first be downloaded, and otherwise runs install -> decode -> enhance ->
    // encode off the UI thread. No-ops while a run is already in flight.
    Q_INVOKABLE void applyEnhance();

    // Accepts the download implied by the consent prompt and starts the run.
    Q_INVOKABLE void confirmDownload();

    // Declines the download: returns to Idle and forgets the consent prompt.
    Q_INVOKABLE void declineDownload();

    // Asks the running worker to stop. `running` stays true until the worker
    // confirms, so the pane's progress keeps showing until then.
    Q_INVOKABLE void cancel();

    // The thread the most recent run's worker used, or 0 before any run.
    quintptr workerThreadId() const { return workerThreadId_.load(); }

signals:
    void clipIdChanged();
    void factorChanged();
    void stateChanged();
    void runningChanged();
    void progressChanged();
    void stageChanged();
    void errorChanged();
    void consentChanged();
    // The run's outcome, on the controller's thread. `ok` is false for a
    // refusal, a cancel, a failure, or a refused apply; `error` carries the
    // reason (empty on success).
    void finished(bool ok, const QString &error);
    // Undo/redo/dirty changed after a successful apply, so the toolbar stays
    // in step with the command the controller added.
    void historyChanged();
    // The timeline's structure changed: the shell reloads the engine/timeline.
    void projectEdited();

private:
    // The selected clip on the active timeline, or null when none/unknown.
    const genesis::project::Clip *clip() const;

    // Refuses a run: parks the reason and emits the failure signals.
    void refuse(const QString &reason);

    // Starts the worker for the already-resolved run (members runEntry_,
    // runClipId_, runMediaPath_, runEncodeRequest_, runOutput_, runFactor_).
    void startRun();

    // The worker's progress callback (on the worker thread): parks the latest
    // fraction and stage and posts at most one queued update, so a fast run
    // never piles up one event per report.
    void reportProgress(double fraction, const QString &stage);

    // Posts finishEnhance onto the controller's thread.
    void postFinish(bool ok, const std::string &error);

    // Runs on the controller's thread when the worker's outcome lands: folds
    // the outcome into the scheduler and the properties, applies the command,
    // emits the signals, then joins the worker.
    void finishEnhance(bool ok, const QString &error);

    // Applies ReplaceClipMedia with the resolved output. False when the editor
    // refused it (the reason lands in `refusal`).
    bool applyEnhanceCommand(QString &refusal);

    // Whether the resolved model file exists on disk (a cheap existence check;
    // the authoritative digest verify runs inside ai::install on the worker).
    bool modelPresent() const;

    void setState(const QString &value);
    void setRunning(bool value);
    void setProgress(double value);
    void setStage(const QString &value);
    void setError(const QString &value);

    genesis::project::Editor *editor_ = nullptr;
    QString clipId_;
    int factor_ = 2;

    QString state_ = QStringLiteral("Idle");
    bool running_ = false;
    double progress_ = 0.0;
    QString stage_;
    QString error_;

    QString consentName_;
    QString consentSize_;
    QString consentLicense_;
    QString consentUrl_;

    // The resolved run's parameters, fixed at applyEnhance (the worker and the
    // apply both read them; a clip moved or re-selected mid-run still applies
    // to the clip it was started on).
    genesis::ai::ModelEntry runEntry_;
    std::string runClipId_;
    std::string runMediaPath_;
    EncodeRequest runEncodeRequest_;
    genesis::project::NewMedia runOutput_;
    std::uint32_t runFactor_ = 2;
    std::uint64_t runEstimatedFrames_ = 0;

    std::filesystem::path model_root_;
    std::filesystem::path output_root_;
    genesis::ai::Fetcher fetcher_;
    Installer installer_;
    FrameSource frameSource_;
    EnhanceFn enhanceFn_;
    Encoder encoder_;

    // The caller-pumped job scheduler owns the run's state machine; one run at
    // a time, driven from the controller's thread only, exactly as the cutout
    // controller drives its run.
    genesis::jobs::Scheduler scheduler_{ 1 };
    genesis::jobs::JobId runId_ = 0;

    // Cross-thread flags: the worker reads cancelRequested_ and writes
    // workerThreadId_; the controller writes cancelRequested_ from cancel().
    std::atomic<bool> cancelRequested_{ false };
    std::atomic<quintptr> workerThreadId_{ 0 };

    // Progress coalescing: the latest fraction/stage parked by the worker, and
    // a gate so at most one queued progress event is in flight at a time. The
    // mutex guards the pair; the atomic is the in-flight gate.
    std::mutex progressMutex_;
    double pendingFraction_ = 0.0;
    QString pendingStage_;
    std::atomic<bool> progressQueued_{ false };

    std::thread workerThread_;
};
