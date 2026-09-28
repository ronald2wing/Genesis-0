// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/controllers/ExportController.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <QMetaObject>
#include <QThread>

#include "adapters/engine/Export.h"
#include "adapters/engine/ges/media/Reverse.h"
#include "core/ClipTimeline.h"
#include "project/Editor.h"
#include "render/ExportPresets.h"
#include "render/ExportSpec.h"
#include "render/Flatten.h"
#include "render/Resolve.h"

namespace engine = genesis::adapters::engine;
namespace ges = genesis::adapters::engine::ges;
namespace jobs = genesis::jobs;
namespace render = genesis::render;
using genesis::core::FrameRate;
using genesis::core::Rational;

ExportController::ExportController(genesis::project::Editor *editor, QObject *parent)
    : QObject(parent), editor_(editor)
{
    for (const render::ExportPreset &preset : render::export_presets()) {
        presets_ << QString::fromUtf8(preset.name.data(),
                                      static_cast<qsizetype>(preset.name.size()));
    }
    quality_ = qualityHint();
}

ExportController::~ExportController()
{
    // A controller destroyed mid-run must not free its members under the
    // worker: ask it to stop, then join. The adapter's watchdog bounds how
    // long a genuinely stuck render can hold this (its span plus a fixed
    // margin), so the join cannot block forever. Cancelling the scheduler job
    // is a no-op before the first run (`runId_` is 0 and never minted).
    cancelRequested_.store(true);
    scheduler_.cancel(runId_);
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

void ExportController::setPreset(const QString &value)
{
    if (preset_ == value) {
        return;
    }
    preset_ = value;
    const QString hint = qualityHint();
    if (quality_ != hint) {
        quality_ = hint;
        emit qualityChanged();
    }
    emit presetChanged();
}

void ExportController::setWidth(int value)
{
    if (width_ == value) {
        return;
    }
    width_ = value;
    emit widthChanged();
}

void ExportController::setHeight(int value)
{
    if (height_ == value) {
        return;
    }
    height_ = value;
    emit heightChanged();
}

void ExportController::setRateNum(qint64 value)
{
    if (rateNum_ == value) {
        return;
    }
    rateNum_ = value;
    emit rateNumChanged();
}

void ExportController::setRateDen(qint64 value)
{
    if (rateDen_ == value) {
        return;
    }
    rateDen_ = value;
    emit rateDenChanged();
}

void ExportController::setOutputPath(const QString &value)
{
    if (outputPath_ == value) {
        return;
    }
    outputPath_ = value;
    emit outputPathChanged();
}

void ExportController::setCacheRoot(const std::filesystem::path &value)
{
    cache_root_ = value;
}

QString ExportController::qualityHint() const
{
    const std::optional<render::ExportPreset> chosen = render::export_preset(preset_.toStdString());
    if (!chosen) {
        return QString();
    }
    return QString::fromUtf8(chosen->quality.data(),
                             static_cast<qsizetype>(chosen->quality.size()));
}

render::ExportSpec ExportController::makeSpec(const std::vector<render::ExportClip> &flat) const
{
    // The preset's codec and sound-codec families; the form's size/rate are
    // the profile override, and the output path and clip list are this
    // controller's. An unknown name (not reachable from the dialog) falls back
    // to the engine defaults rather than a half-formed spec.
    const std::optional<render::ExportPreset> chosen = render::export_preset(preset_.toStdString());
    render::ExportSpec spec = chosen ? render::spec_from(*chosen) : render::ExportSpec{ };
    spec.output = outputPath_.toStdString();
    spec.width = static_cast<std::uint32_t>(width_);
    spec.height = static_cast<std::uint32_t>(height_);
    spec.rate_num = rateNum_;
    spec.rate_den = rateDen_;
    spec.clips = flat;
    return spec;
}

QStringList ExportController::validate() const
{
    QStringList problems;
    if (editor_ == nullptr) {
        problems << QStringLiteral("no project");
        return problems;
    }
    const std::vector<render::ExportClip> flat =
            render::flatten_timeline(editor_->project(), std::nullopt);
    const render::ExportSpec spec = makeSpec(flat);
    for (const std::string &problem : render::validate(spec)) {
        problems << QString::fromStdString(problem);
    }
    return problems;
}

void ExportController::startExport()
{
    if (running_) {
        return;
    }
    if (editor_ == nullptr) {
        setError(QStringLiteral("no project"));
        return;
    }
    const QStringList problems = validate();
    if (!problems.isEmpty()) {
        setError(problems.join(QStringLiteral("; ")));
        return;
    }

    // Pause playback before the export starts. The preview and the export are
    // separate GES pipelines over the same media; leaving the preview rolling
    // would contend for the decoders and, on a single-encoder machine, the
    // hardware encoder. The monitor therefore freezes on its last frame for
    // the duration of the export and stays paused afterwards until the user
    // presses play again - the choice, and the reason, in one place.
    if (pausePlayback_) {
        pausePlayback_();
    }

    const std::vector<render::ExportClip> flat =
            render::flatten_timeline(editor_->project(), std::nullopt);
    const render::ExportSpec spec = makeSpec(flat);

    // Queue the run on the scheduler, then start it. With max_concurrent = 1
    // and the `running_` guard holding off a second submit, this is the one
    // Running job - matching the single-export guarantee the controller always
    // had. The scheduler lives on this (the controller's) thread; the worker
    // below never touches it, only the queued handlers do.
    runId_ = scheduler_.submit(jobs::JobRequest{ });
    scheduler_.acquire();

    cancelRequested_.store(false);
    workerThreadId_.store(0);
    pendingFraction_.store(0.0);
    progressQueued_.store(false);
    setProgress(0.0);
    setError(QString());
    setStatus(QStringLiteral("Exporting"));
    setRunning(true);

    // R4: an export blocks, so it must not run on the UI thread. The worker
    // owns the spec and the graph by value and reaches back into the controller
    // only through the cancellation flag and queued invokes, so no controller
    // member is touched off-thread. The thread is joined in finishExport, so it
    // is never reassigned while still running.
    //
    // The graph is built here, on the worker, rather than on the UI thread,
    // because a reversed clip's copy must exist before `build_timeline` finds
    // it fresh - and generating that copy is blocking. `ensure_reverses` runs
    // first, off the UI thread, then the graph is built against the same spec
    // the form was validated with, so size and rate can never disagree.
    workerThread_ = std::thread([this, spec = std::move(spec), cache_root = cache_root_]() mutable {
        workerThreadId_.store(reinterpret_cast<quintptr>(QThread::currentThread()));
        if (!cache_root.empty()) {
            ges::ensure_reverses(spec.clips, cache_root, [this]() {
                return cancelRequested_.load(std::memory_order_relaxed);
            });
        }
        const std::optional<std::filesystem::path> reverse_root = cache_root.empty()
                ? std::nullopt
                : std::optional<std::filesystem::path>(cache_root);

        // The BuiltTimeline carries the output frame and rate the render
        // scales to: export_timeline reads the timeline's width/height/rate
        // for its video restriction caps, so the graph is built from the
        // same form numbers the spec was validated against - the two can
        // never disagree about size.
        render::ExportRequest request;
        request.width = spec.width;
        request.height = spec.height;
        request.rate_num = spec.rate_num;
        request.rate_den = spec.rate_den;
        render::BuiltTimeline built = render::build_timeline(
                request, FrameRate(Rational(spec.rate_num, spec.rate_den)),
                std::span<const render::ExportClip>(spec.clips), { }, { }, { }, reverse_root);

        const auto progress = [this](const engine::ExportProgress &report) {
            reportProgress(report.fraction);
        };
        const auto cancelled = [this]() {
            return cancelRequested_.load(std::memory_order_relaxed);
        };
        const engine::ExportOutcome outcome =
                engine::export_timeline(spec, built, progress, cancelled);
        QMetaObject::invokeMethod(
                this,
                [this, outcome]() {
                    finishExport(outcome.ok, QString::fromStdString(outcome.error),
                                 outcome.duration.as_double());
                },
                Qt::QueuedConnection);
    });
}

void ExportController::cancelExport()
{
    if (!running_) {
        return;
    }
    // Flag the job on the scheduler (cancel wins over the result when the
    // worker completes) and set the atomic the worker's cancellation callback
    // polls. The worker unwinds within its tick interval and the adapter
    // removes the partial file. `running` stays true until finishExport lands,
    // so the dialog keeps showing the progress bar while the worker confirms
    // the stop.
    scheduler_.cancel(runId_);
    cancelRequested_.store(true, std::memory_order_relaxed);
}

void ExportController::reportProgress(double fraction)
{
    // Runs on the worker thread. Park the latest fraction, then post at most
    // one queued update: if a handoff is already in flight, the next report
    // only overwrites the parked value and queues nothing, so a render that
    // reports faster than the UI paints never piles up one event per report.
    pendingFraction_.store(fraction, std::memory_order_relaxed);
    if (!progressQueued_.exchange(true, std::memory_order_acq_rel)) {
        QMetaObject::invokeMethod(
                this,
                [this]() {
                    progressQueued_.store(false, std::memory_order_release);
                    const double parked = pendingFraction_.load(std::memory_order_relaxed);
                    // The scheduler owns the run's progress (clamped to 0..1); the
                    // property mirrors it for the dialog's bar.
                    scheduler_.report_progress(runId_, jobs::Progress{ parked, "exporting" });
                    setProgress(parked);
                },
                Qt::QueuedConnection);
    }
}

void ExportController::finishExport(bool ok, const QString &error, double durationSeconds)
{
    // Runs on the controller's thread. Drive the scheduler to its terminal
    // state, then fold the outcome into the properties and emit the signals,
    // then join the worker - which has already posted this call and is
    // returning - so the next startExport can safely reuse workerThread_.
    if (ok) {
        scheduler_.complete(runId_, jobs::JobResult{ });
    } else if (error == QStringLiteral("cancelled")) {
        // cancelExport already flagged the job, so completing it now lands it
        // Cancelled (cancel wins, the result is discarded).
        scheduler_.complete(runId_, jobs::JobResult{ });
    } else {
        scheduler_.fail(runId_, error.toStdString());
    }

    if (ok) {
        setProgress(1.0);
        setStatus(QStringLiteral("Done"));
        setError(QString());
    } else if (error == QStringLiteral("cancelled")) {
        // The adapter's own cancellation string, so the report is honest even
        // if cancelExport() and the outcome raced.
        setStatus(QStringLiteral("Cancelled"));
        setError(QStringLiteral("cancelled"));
    } else {
        setStatus(QStringLiteral("Failed"));
        setError(error);
    }
    setRunning(false);
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    emit finished(ok, error, durationSeconds);
}

void ExportController::setRunning(bool value)
{
    if (running_ == value) {
        return;
    }
    running_ = value;
    emit runningChanged();
}

void ExportController::setProgress(double value)
{
    if (progress_ == value) {
        return;
    }
    progress_ = value;
    emit progressChanged();
}

void ExportController::setStatus(const QString &value)
{
    if (status_ == value) {
        return;
    }
    status_ = value;
    emit statusChanged();
}

void ExportController::setError(const QString &value)
{
    if (error_ == value) {
        return;
    }
    error_ = value;
    emit errorChanged();
}
