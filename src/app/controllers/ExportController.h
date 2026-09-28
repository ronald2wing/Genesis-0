// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The studio shell's export controller: the thin, testable layer that turns
// the dialog's form values into one blocking render run, driven off the UI
// thread (R4). It owns no media and no engine session - only the mapping from
// a chosen export preset plus the form's size/rate to a `render::ExportSpec`,
// the worker thread that runs `export_timeline` under the job Scheduler, and
// the marshalling of progress/outcome back to the controller's thread. The QML
// dialog binds to the properties below and calls the three Q_INVOKABLEs; the
// controller itself never touches QML.

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <atomic>
#include <filesystem>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

#include "jobs/Scheduler.h"

namespace genesis::project {
class Editor;
}

namespace genesis::render {
struct ExportClip;
struct ExportSpec;
} // namespace genesis::render

class ExportController : public QObject
{
    Q_OBJECT

    // ---- The form (write from the dialog, read by validate/startExport). ----

    // The selected export preset: a named target ("vp8", "h264", ...) whose
    // codec and sound-codec families `validate`/`startExport` resolve to an
    // export spec. The dialog only offers names from `presets`, so a value here
    // is always resolvable.
    Q_PROPERTY(QString preset READ preset WRITE setPreset NOTIFY presetChanged)
    // Every preset the dialog offers, in menu order.
    Q_PROPERTY(QStringList presets READ presets CONSTANT)
    // The selected preset's medium-quality hint ("23 crf", "3 Mb/s", ...), for
    // the dialog to display. Read-only: the preset owns it.
    Q_PROPERTY(QString quality READ quality NOTIFY qualityChanged)
    // Output frame size in pixels. Independent of the preset: it overrides the
    // preset's default frame (the "profile override" the resolver leaves to its
    // caller).
    Q_PROPERTY(int width READ width WRITE setWidth NOTIFY widthChanged)
    Q_PROPERTY(int height READ height WRITE setHeight NOTIFY heightChanged)
    // Output frame rate as an exact fraction, so 29.97 is 30000/1001 rather
    // than a rounded double. The same profile override as width/height.
    Q_PROPERTY(qint64 rateNum READ rateNum WRITE setRateNum NOTIFY rateNumChanged)
    Q_PROPERTY(qint64 rateDen READ rateDen WRITE setRateDen NOTIFY rateDenChanged)
    // Where the finished file is written. Non-empty by default so the dialog
    // opens valid; AppShell overrides it with a media-relative path.
    Q_PROPERTY(QString outputPath READ outputPath WRITE setOutputPath NOTIFY outputPathChanged)

    // ---- The run's read-only view (the dialog binds to these). ----

    // True while a worker owns a render. Gates Export (off) and Cancel (on).
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    // How much of the timeline has been written, 0..1.
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    // "Idle", "Exporting", "Done", "Cancelled" or "Failed".
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    // The last problem: a validation message or the engine's refusal.
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)

public:
    explicit ExportController(genesis::project::Editor *editor, QObject *parent = nullptr);
    ~ExportController() override;

    QString preset() const { return preset_; }
    QStringList presets() const { return presets_; }
    QString quality() const { return quality_; }
    int width() const { return width_; }
    int height() const { return height_; }
    qint64 rateNum() const { return rateNum_; }
    qint64 rateDen() const { return rateDen_; }
    QString outputPath() const { return outputPath_; }
    bool running() const { return running_; }
    double progress() const { return progress_; }
    QString status() const { return status_; }
    QString error() const { return error_; }

    void setPreset(const QString &value);
    void setWidth(int value);
    void setHeight(int value);
    void setRateNum(qint64 value);
    void setRateDen(qint64 value);
    void setOutputPath(const QString &value);

    // The problems `render::validate` reports for the current form and the
    // current project, so the dialog can disable Export and show why. Empty
    // when the form is runnable. Flattens the project on each call; cheap at
    // this clip count, and it keeps the spec the one source of truth.
    Q_INVOKABLE QStringList validate() const;

    // Builds the spec and the host graph from the form, pauses playback, and
    // hands the blocking render to a worker thread (R4). No-ops while already
    // running, and refuses (leaving a message in `error`) when the form does
    // not validate or there is no project.
    Q_INVOKABLE void startExport();

    // Sets the flag the worker's cancellation callback polls. The worker
    // unwinds within its tick interval and reports a cancelled outcome; the
    // adapter removes the partial file, so a cancel leaves nothing behind.
    // `running` stays true until the outcome lands, so the dialog's progress
    // bar keeps showing until the worker confirms it stopped.
    Q_INVOKABLE void cancelExport();

    // The thread the most recent run's worker used, or 0 before any run. For
    // the tests to prove the render actually left the controller's thread.
    quintptr workerThreadId() const { return workerThreadId_.load(); }

    // The playback pause hook: invoked once before the export starts, so the
    // two GES pipelines (preview and export) never contend for decoders or,
    // on single-encoder machines, the hardware encoder. AppShell wires it to
    // the PlaybackController::pause; a headless test leaves it empty.
    void setPausePlayback(std::function<void()> hook) { pausePlayback_ = std::move(hook); }

    // Where the reversed copies of reversed clips live, set by the composition
    // root from the same cache root the media bin and titles use. Empty until
    // set, which disables reverse generation (a caller from before the field
    // existed). The worker generates any missing reverses off the UI thread
    // before building the graph, so a reversed clip exports backwards.
    void setCacheRoot(const std::filesystem::path &value);
    std::filesystem::path cacheRoot() const { return cache_root_; }

signals:
    void presetChanged();
    void qualityChanged();
    void widthChanged();
    void heightChanged();
    void rateNumChanged();
    void rateDenChanged();
    void outputPathChanged();
    void runningChanged();
    void progressChanged();
    void statusChanged();
    void errorChanged();
    // The run's outcome, on the controller's thread. `durationSeconds` is the
    // written span on success, zero otherwise.
    void finished(bool ok, const QString &error, double durationSeconds);

private:
    // The spec the form describes: the selected preset's codec/audio families,
    // overridden by the form's size/rate (the profile override), the output
    // path, and the flattened clips. `validate` and `startExport` share it so
    // the spec they check is the one they render.
    genesis::render::ExportSpec
    makeSpec(const std::vector<genesis::render::ExportClip> &flat) const;

    // The selected preset's quality hint, or empty when the name does not
    // resolve.
    QString qualityHint() const;

    // The worker's progress callback (on the worker thread): parks the latest
    // fraction and posts at most one queued update, so a fast render never
    // piles up one event per report. Mirrors MonitorItem::stageFrame's
    // latest-wins discipline.
    void reportProgress(double fraction);

    // Runs on the controller's thread when the worker's outcome lands: folds
    // the outcome into the scheduler and the properties and emits the signals,
    // then joins the worker (which has already posted this very call and is
    // returning). The worker unpacks the adapter's outcome at the call site, so
    // no adapter type crosses into this header.
    void finishExport(bool ok, const QString &error, double durationSeconds);

    void setRunning(bool value);
    void setProgress(double value);
    void setStatus(const QString &value);
    void setError(const QString &value);

    genesis::project::Editor *editor_ = nullptr;
    std::function<void()> pausePlayback_;
    std::filesystem::path cache_root_;

    // The caller-pumped job scheduler owns the run's state machine (queued ->
    // running -> done/failed/cancelled), progress clamping and cancel-wins
    // semantics; one export at a time (max_concurrent = 1) matches the
    // controller's single run. Driven from the controller's thread only, so it
    // never needs internal synchronisation. Its JobRequest/JobResult payloads
    // are the AI job kinds (see docs/decisions/ai-provider.md) - an export is
    // none of them, and the contract is frozen - so the export's real work
    // (spec + graph) is captured by the worker, not the job; the request and
    // result are inert placeholders and only the state machine is used.
    genesis::jobs::Scheduler scheduler_{ 1 };
    genesis::jobs::JobId runId_ = 0;

    QStringList presets_;
    QString preset_ = QStringLiteral("vp8");
    QString quality_;
    int width_ = 1920;
    int height_ = 1080;
    qint64 rateNum_ = 30;
    qint64 rateDen_ = 1;
    QString outputPath_ = QStringLiteral("export.webm");

    bool running_ = false;
    double progress_ = 0.0;
    QString status_ = QStringLiteral("Idle");
    QString error_;

    // Cross-thread flags: the worker reads cancelRequested_ from its
    // cancellation callback and writes workerThreadId_ once at start; the
    // controller writes cancelRequested_ from cancelExport(). The scheduler
    // itself stays on the controller's thread; only these atomics and the
    // queued invokes cross the thread boundary.
    std::atomic<bool> cancelRequested_{ false };
    std::atomic<quintptr> workerThreadId_{ 0 };

    // Progress coalescing: the latest fraction parked by the worker, and a
    // gate so at most one queued progress event is in flight at a time.
    std::atomic<double> pendingFraction_{ 0.0 };
    std::atomic<bool> progressQueued_{ false };

    std::thread workerThread_;
};
