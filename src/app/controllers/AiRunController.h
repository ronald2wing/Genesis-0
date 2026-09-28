// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Abstract base for a controller that runs one long AI job off the UI thread.
// Owns the run lifecycle: submit, cancel, progress coalescing, terminal-state
// fold, and the consent gate. Subclasses contribute the real work (startRun,
// available, statusText), the apply callback, and the result payload.
//
// Not a QML type: properties stay on the concrete subclasses so QML's
// stringly-coupled names never move.

#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

#include "jobs/Job.h"
#include "jobs/Scheduler.h"

namespace genesis::project {
class Editor;
} // namespace genesis::project

namespace genesis::app {

class AiRunController : public QObject
{
    Q_OBJECT

public:
    explicit AiRunController(genesis::project::Editor *editor, QObject *parent = nullptr);
    ~AiRunController() override;

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

    quintptr workerThreadId() const { return workerThreadId_.load(); }

    Q_INVOKABLE void confirmDownload();
    Q_INVOKABLE void declineDownload();
    Q_INVOKABLE void cancel();

signals:
    void stateChanged();
    void runningChanged();
    void progressChanged();
    void stageChanged();
    void errorChanged();
    void consentChanged();
    void finished(bool ok, const QString &error);
    void historyChanged();
    void projectEdited();

protected:
    void refuse(const QString &reason);
    void reportProgress(double fraction, const QString &stage);

    void setState(const QString &value);
    void setRunning(bool value);
    void setProgress(double value);
    void setStage(const QString &value);
    void setError(const QString &value);

    // The shared preamble of startRun: submits the request, acquires the
    // scheduler, and resets the run's cross-thread state. The subclass calls
    // this, then starts its worker thread.
    void beginRun(genesis::jobs::JobRequest request);

    // The shared terminal-state fold: epoch guard, ok/cancelled/failed
    // branches, worker join, finished signal. The subclass supplies the
    // apply callback (its apply* method) and the result builder.
    using ApplyFn = std::function<bool(QString &refusal)>;
    using ResultFn = std::function<genesis::jobs::JobResult()>;
    void finishRun(bool ok, const QString &error, const ApplyFn &apply, const ResultFn &result);

    // ---- Subclass hooks ----

    virtual bool available() const = 0;
    virtual QString statusText() const = 0;
    virtual void startRun() = 0;
    virtual bool modelPresent() const = 0;
    virtual const char *discardLog() const = 0;
    // When the run succeeds but must not call apply (e.g. a regenerate-only
    // run that re-keys masks without adding an undo step), the subclass
    // returns false and finishRun completes without the apply callback,
    // emitting projectEdited to reload the engine.
    virtual bool applyOnSuccess() const { return true; }

    genesis::project::Editor *editor_ = nullptr;

    // The caller-pumped job scheduler owns the run's state machine; one run at
    // a time, driven from the controller's thread only.
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

    QString state_ = QStringLiteral("Idle");
    bool running_ = false;
    double progress_ = 0.0;
    QString stage_;
    QString error_;

    QString consentName_;
    QString consentSize_;
    QString consentLicense_;
    QString consentUrl_;

    std::uint64_t runEpoch_ = 0;
};

} // namespace genesis::app