// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/controllers/AiRunController.h"

#include <mutex>
#include <utility>

#include <QMetaObject>
#include <QString>
#include <QThread>

#include "app/support/AppLog.h"
#include "project/Editor.h"

namespace genesis::app {

AiRunController::AiRunController(genesis::project::Editor *editor, QObject *parent)
    : QObject(parent), editor_(editor)
{
}

AiRunController::~AiRunController()
{
    // A controller destroyed mid-run must not free its members under the
    // worker: ask it to stop, then join. Cancelling the scheduler job is a
    // no-op before the first run (runId_ is 0 and never minted).
    cancelRequested_.store(true);
    scheduler_.cancel(runId_);
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

void AiRunController::refuse(const QString &reason)
{
    setError(reason);
    setState(QStringLiteral("Failed"));
    emit finished(false, reason);
}

void AiRunController::confirmDownload()
{
    if (state_ != QStringLiteral("Consent")) {
        return;
    }
    startRun();
}

void AiRunController::declineDownload()
{
    if (state_ != QStringLiteral("Consent")) {
        return;
    }
    consentName_.clear();
    consentSize_.clear();
    consentLicense_.clear();
    consentUrl_.clear();
    setState(QStringLiteral("Idle"));
    emit consentChanged();
}

void AiRunController::cancel()
{
    if (!running_) {
        return;
    }
    // Flag the job on the scheduler (cancel wins over the result when the
    // worker completes) and set the atomic the worker polls. `running` stays
    // true until the finish path lands, so the pane keeps showing progress
    // while the worker confirms the stop.
    scheduler_.cancel(runId_);
    cancelRequested_.store(true, std::memory_order_relaxed);
}

void AiRunController::reportProgress(double fraction, const QString &stage)
{
    // Runs on the worker thread. Park the latest fraction and stage, then post
    // at most one queued update: if a handoff is already in flight, the next
    // report only overwrites the parked values and queues nothing, so a run
    // that reports faster than the UI paints never piles up one event per
    // report.
    {
        std::lock_guard<std::mutex> lock(progressMutex_);
        pendingFraction_ = fraction;
        pendingStage_ = stage;
    }
    if (!progressQueued_.exchange(true, std::memory_order_acq_rel)) {
        QMetaObject::invokeMethod(
                this,
                [this]() {
                    progressQueued_.store(false, std::memory_order_release);
                    double fraction = 0.0;
                    QString stage;
                    {
                        std::lock_guard<std::mutex> lock(progressMutex_);
                        fraction = pendingFraction_;
                        stage = pendingStage_;
                    }
                    // The scheduler owns the run's progress (clamped to 0..1);
                    // the properties mirror it for the pane's bar.
                    scheduler_.report_progress(
                            runId_, genesis::jobs::Progress{ fraction, stage.toStdString() });
                    setProgress(fraction);
                    setStage(stage);
                },
                Qt::QueuedConnection);
    }
}

void AiRunController::beginRun(genesis::jobs::JobRequest request)
{
    // Queue the run on the scheduler, then start it. With max_concurrent = 1
    // and the `running_` guard holding off a second submit, this is the one
    // Running job. The scheduler lives on this (the controller's) thread; the
    // worker below never touches it, only the queued handlers do.
    runId_ = scheduler_.submit(std::move(request));
    scheduler_.acquire();

    cancelRequested_.store(false, std::memory_order_relaxed);
    workerThreadId_.store(0);
    {
        std::lock_guard<std::mutex> lock(progressMutex_);
        pendingFraction_ = 0.0;
        pendingStage_.clear();
    }
    progressQueued_.store(false);
    setProgress(0.0);
    setStage(QString());
    setError(QString());
    setState(QStringLiteral("Running"));
    setRunning(true);
}

void AiRunController::finishRun(bool ok, const QString &error, const ApplyFn &apply,
                                const ResultFn &result)
{
    // Runs on the controller's thread. Drive the scheduler to its terminal
    // state, fold the outcome into the properties and emit the signals, then
    // join the worker - which has already posted this call and is returning -
    // so the next run can safely reuse workerThread_.
    bool final_ok = ok;
    QString final_error = error;

    // The project was swapped out from under this run (a load bumped the
    // editor's epoch): discard the result rather than apply it to the wrong
    // project. The epoch check runs BEFORE the apply callback, and the worker
    // is joined on every exit path including the discard branch - this is the
    // ordering every AI controller's test_project_change_discards case pins.
    if (ok && editor_ != nullptr && editor_->epoch() != runEpoch_) {
        genesis::app::app_log_at(genesis::app::LogLevel::Warn, "%s", discardLog());
        scheduler_.complete(runId_, genesis::jobs::JobResult{ });
        setState(QStringLiteral("Cancelled"));
        setError(QStringLiteral("project changed"));
        setRunning(false);
        if (workerThread_.joinable()) {
            workerThread_.join();
        }
        emit finished(false, QStringLiteral("project changed"));
        return;
    }

    if (ok) {
        if (applyOnSuccess()) {
            QString refusal;
            if (apply(refusal)) {
                scheduler_.complete(runId_, result());
                setProgress(1.0);
                setState(QStringLiteral("Done"));
                setError(QString());
            } else {
                final_ok = false;
                final_error = refusal;
                scheduler_.fail(runId_, refusal.toStdString());
                setState(QStringLiteral("Failed"));
                setError(refusal);
            }
        } else {
            // The run succeeded but the controller asked to skip apply: the
            // result lands (masks were re-keyed in place, an undo step already
            // exists), and projectEdited triggers an engine reload.
            scheduler_.complete(runId_, result());
            setProgress(1.0);
            setState(QStringLiteral("Done"));
            setError(QString());
            emit projectEdited();
        }
    } else if (final_error == QStringLiteral("cancelled")) {
        // cancel() already flagged the job, so completing it now lands it
        // Cancelled (cancel wins, the result is discarded).
        scheduler_.complete(runId_, genesis::jobs::JobResult{ });
        setState(QStringLiteral("Cancelled"));
        setError(QStringLiteral("cancelled"));
    } else {
        scheduler_.fail(runId_, final_error.toStdString());
        setState(QStringLiteral("Failed"));
        setError(final_error);
    }

    setRunning(false);
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    emit finished(final_ok, final_error);
}

void AiRunController::setState(const QString &value)
{
    if (state_ == value) {
        return;
    }
    state_ = value;
    emit stateChanged();
}

void AiRunController::setRunning(bool value)
{
    if (running_ == value) {
        return;
    }
    running_ = value;
    emit runningChanged();
}

void AiRunController::setProgress(double value)
{
    if (progress_ == value) {
        return;
    }
    progress_ = value;
    emit progressChanged();
}

void AiRunController::setStage(const QString &value)
{
    if (stage_ == value) {
        return;
    }
    stage_ = value;
    emit stageChanged();
}

void AiRunController::setError(const QString &value)
{
    if (error_ == value) {
        return;
    }
    error_ = value;
    emit errorChanged();
}

} // namespace genesis::app