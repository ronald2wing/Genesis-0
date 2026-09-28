// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The studio shell's AI cutout controller: the thin, testable layer that turns
// one "cutout" request into a single undoable SetClipCutout over the selected
// clip. It owns no media and no engine - only the consent gate, the
// download/mask-generation worker, and the mapping from a chosen subject to the
// model that masks it and the command that applies it. The QML pane binds to
// the properties below and calls the Q_INVOKABLEs; the controller never
// touches QML. Its public surface (state, progress, error, consent) mirrors
// AiController's exactly, so the UI can share one dialog shape for both.
//
// Flow. applyCutout(subject) refuses when the ONNX segmentation runtime is not
// compiled in or no clip is selected; it maps the subject to its model (RVM for
// person matting, IS-Net for object segmentation) and, when that model is not
// yet on disk, parks in Consent until confirmDownload() or declineDownload().
// Otherwise it starts one worker that (1) installs the model through ai::install
// (with the consent it implied), (2) decodes the clip's video to RGB frames,
// (3) segments each frame into an alpha mask through the MaskDriver (which
// reuses any fresh mask already on disk), and (4) hands the outcome back to the
// controller thread, where it becomes one SetClipCutout. Cancel flags the job
// and the atomic the worker polls; the run reports Cancelled when the worker
// next completes.
//
// Mapping. A subject becomes a Cutout with mode Auto and that subject (the
// render path's mask_target() then names the same model id and jobs::Subject
// the driver keyed masks with, so masks are found automatically). The driver
// generates one mask per source frame (stride 1), matching the render path's
// pinned MaskSpec, so a frame whose mask exists reads cut and one whose mask
// was never generated reads absent. clearCutout() applies SetClipCutout with no
// cutout - a model-free disable that needs no consent and no worker.

#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ai/ModelStore.h"
#include "ai/vision/MaskDriver.h"
#include "ai/vision/SegmentationProvider.h"
#include "jobs/Job.h"
#include "jobs/Scheduler.h"
#include "project/model/Cutout.h"

namespace genesis::project {
class Editor;
struct Clip;
} // namespace genesis::project

class CutoutController : public QObject
{
    Q_OBJECT

    // ---- Capability (static at runtime). ----

    // Whether the ONNX segmentation runtime was compiled in. False in the
    // default build, where applyCutout refuses with a clear reason.
    Q_PROPERTY(bool available READ available CONSTANT)
    // "enabled" or "disabled": the runtime's own state, for the pane to show.
    Q_PROPERTY(QString statusText READ statusText CONSTANT)

    // ---- Target. ----

    // The clip being cut out. Set by the shell, which already owns selection;
    // empty means nothing is selected and applyCutout refuses.
    Q_PROPERTY(QString clipId READ clipId WRITE setClipId NOTIFY clipIdChanged)

    // ---- The run's read-only view (the pane binds to these). ----

    // "Idle", "Consent", "Running", "Done", "Cancelled" or "Failed".
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    // True while a worker owns a run.
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    // How much of the whole run has finished, 0..1 (download then mask
    // generation).
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    // The current stage: "downloading model", "segmenting", "done".
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
    // a fake that hands back a canned path - the pinned model's real bytes
    // (and its SHA-256) cannot be fabricated in a unit test, and the controller
    // must stay testable without a download. The default is the real ai::install.
    using Installer = std::function<genesis::ai::InstallResult(
            const std::filesystem::path &, const genesis::ai::ModelEntry &,
            const genesis::ai::Fetcher &, const genesis::ai::Progress &)>;

    // A seam for frame decoding: pumps decoded RGB frames into the driver's
    // sink. Tests inject a fake so no GStreamer decode runs; the default wraps
    // ges::extract_video_frames.
    using FrameSource = genesis::ai::vision::FrameSource;

    // A seam for segmentation: one frame in, one alpha mask out. Tests inject a
    // fake returning canned alpha so no model and no runtime load; the default
    // loads the SegmentationProvider once and reuses its session per frame.
    using SegmentFn = genesis::ai::vision::SegmentFn;

    explicit CutoutController(genesis::project::Editor *editor, QObject *parent = nullptr);
    ~CutoutController() override;

    bool available() const { return genesis::ai::vision::SegmentationProvider::available(); }
    // The runtime's own state, but human-readable: the provider reports a bare
    // "enabled"/"disabled", so a disabled runtime is expanded to the same
    // reason applyCutout() refuses with.
    QString statusText() const
    {
        if (!available()) {
            return QStringLiteral("disabled: ONNX Runtime not compiled in "
                                  "(rebuild with GENESIS_AI_VISION=ON)");
        }
        const std::string_view status = genesis::ai::vision::SegmentationProvider::status();
        return QString::fromUtf8(status.data(), static_cast<qsizetype>(status.size()));
    }

    QString clipId() const { return clipId_; }
    void setClipId(const QString &clipId);

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
    void setSegmentFn(SegmentFn segment) { segmentFn_ = std::move(segment); }
    // A seam for the smart-brush refinement: refines a frame's base alpha with
    // its smart strokes. Tests inject a fake so no SlimSAM encoder/decoder
    // forward pass runs; the default lazily loads BrushProvider from the
    // installed model paths.
    void setBrushRefineFn(genesis::ai::vision::BrushRefineFn refine)
    {
        brushRefineFn_ = std::move(refine);
    }
    // Where installed models live (<root>/<id>/<version>/<file>). The app sets
    // it to AppDataLocation/models; a test sets a scratch dir.
    void setModelRoot(const std::filesystem::path &root) { model_root_ = root; }
    // Where generated masks live (<root>/*.mask). The app sets it to
    // CacheLocation/masks - the same folder the render path reads.
    void setMaskRoot(const std::filesystem::path &root) { mask_root_ = root; }

    // Starts a cutout run over the selected clip for `subject` ("person" or
    // "object"): refuses when the runtime is disabled, the subject has no
    // model, or the clip has no picture; parks in Consent when the model must
    // first be downloaded; otherwise runs install -> decode -> segment off the
    // UI thread. No-ops while a run is already in flight.
    Q_INVOKABLE void applyCutout(const QString &subject);

    // Removes the selected clip's cutout - a model-free disable applied as a
    // single undo step, with no consent and no worker.
    Q_INVOKABLE void clearCutout();

    // Debounced re-mask: coalesces a burst of stroke commits into one
    // regeneration once the painting pauses (300ms), so a fast stroke drag
    // does not re-segment and re-decode the whole clip per commit.
    Q_INVOKABLE void scheduleRegenerate(const QString &clipId);

    // Re-runs mask generation for `clipId`'s current cutout immediately: its
    // strokes, its revision, and (for smart strokes) a brush refinement. The
    // cutout is already on the clip (AddCutoutStroke placed it), so no new
    // undo step is added - only the masks on disk are (re)generated, and the
    // engine is reloaded so the render path finds them. Refuses when the clip
    // has no cutout, or its subject is Auto (no model yet).
    Q_INVOKABLE void regenerateClip(const QString &clipId);

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

    // The clip named by `id` on the active timeline, or null when unknown.
    const genesis::project::Clip *clipById(const std::string &id) const;

    // Refuses a run: parks the reason and emits the failure signals.
    void refuse(const QString &reason);

    // The models this run installs, in order: the segmentation model, then the
    // smart-brush encoder/decoder when the run refines smart strokes.
    std::vector<genesis::ai::ModelEntry> installList() const;

    // Fills the consent prompt from the models a run installs (name/size/
    // licence/url, aggregated across all of them).
    void setConsentFor(const std::vector<genesis::ai::ModelEntry> &entries);

    // Starts the worker for the already-resolved run (members runEntry_,
    // runSubject_, runClipId_, runMediaPath_, runSourceSize_, runMaskRoot_,
    // runRevision_, runStrokes_, runApply_ and the brush entries).
    void startRun();

    // The worker's progress callback (on the worker thread): parks the latest
    // fraction and stage and posts at most one queued update, so a fast run
    // never piles up one event per report.
    void reportProgress(double fraction, const QString &stage);

    // Posts finishCutout onto the controller's thread.
    void postFinish(bool ok, const std::string &error);

    // Runs on the controller's thread when the worker's outcome lands: folds
    // the outcome into the scheduler and the properties, applies the command,
    // emits the signals, then joins the worker.
    void finishCutout(bool ok, const QString &error);

    // Applies SetClipCutout with the resolved subject. False when the editor
    // refused it (the reason lands in `refusal`).
    bool applyCutoutCommand(QString &refusal);

    // Whether every model this run installs already exists on disk (a cheap
    // existence check; the authoritative digest verify runs inside ai::install
    // on the worker).
    bool modelPresent() const;

    void setState(const QString &value);
    void setRunning(bool value);
    void setProgress(double value);
    void setStage(const QString &value);
    void setError(const QString &value);

    genesis::project::Editor *editor_ = nullptr;
    QString clipId_;

    QString state_ = QStringLiteral("Idle");
    bool running_ = false;
    double progress_ = 0.0;
    QString stage_;
    QString error_;

    QString consentName_;
    QString consentSize_;
    QString consentLicense_;
    QString consentUrl_;

    // The resolved run's parameters, fixed at applyCutout/regenerateClip (the
    // worker and the apply both read them; a clip moved or re-selected mid-run
    // still applies to the clip it was started on).
    genesis::ai::ModelEntry runEntry_;
    genesis::project::Subject runSubject_ = genesis::project::Subject::Auto;
    std::string runClipId_;
    std::string runMediaPath_;
    std::uintmax_t runSourceSize_ = 0;
    std::filesystem::path runMaskRoot_;
    // The stroke revision the masks are keyed by (mask_revision()); empty for
    // an automatic cutout, non-empty for a regenerate of a custom one.
    std::string runRevision_;
    // The smart strokes to refine the base masks with; empty for an automatic
    // cutout (or a custom one painted only with plain discs).
    std::vector<genesis::ai::vision::BrushStroke> runStrokes_;
    // Whether a successful run applies SetClipCutout (applyCutout) or only
    // regenerates masks already keyed to the clip's cutout (regenerateClip).
    bool runApply_ = true;
    // The smart-brush models, present only when the run refines smart strokes.
    std::optional<genesis::ai::ModelEntry> runBrushEncoder_;
    std::optional<genesis::ai::ModelEntry> runBrushDecoder_;

    std::filesystem::path model_root_;
    std::filesystem::path mask_root_;
    genesis::ai::Fetcher fetcher_;
    Installer installer_;
    FrameSource frameSource_;
    SegmentFn segmentFn_;
    genesis::ai::vision::BrushRefineFn brushRefineFn_;

    // The stroke-commit debounce: one regeneration per burst, fired on the
    // controller's thread after the painting pauses.
    QTimer strokeDebounce_;
    QString pendingClipId_;

    // The caller-pumped job scheduler owns the run's state machine; one run at
    // a time, driven from the controller's thread only, exactly as the caption
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
