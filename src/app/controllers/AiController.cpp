// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/controllers/AiController.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QMetaObject>
#include <QString>
#include <QThread>

#include "app/net/QtFetcher.h"
#include "project/Editor.h"
#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/model/Text.h"

namespace ai = genesis::ai;
namespace ges = genesis::adapters::engine::ges;
namespace jobs = genesis::jobs;
using genesis::core::Rational;

namespace {

// The UI -> host seam: seconds (a double) become the host's exact rational. A
// value the rational cannot hold falls back to zero, matching the command
// layer's clamp-not-error posture.
Rational seconds(double value)
{
    if (const auto rational = Rational::approximate(value)) {
        return *rational;
    }
    return Rational::ZERO;
}

// A byte count as a short human string ("147 MB"), for the consent prompt.
QString human_size(std::uintmax_t bytes)
{
    const double value = static_cast<double>(bytes);
    if (bytes >= 1024 * 1024 * 1024) {
        return QStringLiteral("%1 GB").arg(value / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1);
    }
    if (bytes >= 1024 * 1024) {
        return QStringLiteral("%1 MB").arg(value / (1024.0 * 1024.0), 0, 'f', 1);
    }
    if (bytes >= 1024) {
        return QStringLiteral("%1 KB").arg(value / 1024.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 B").arg(bytes);
}

} // namespace

AiController::AiController(genesis::project::Editor *editor, QObject *parent)
    : QObject(parent), editor_(editor)
{
}

AiController::~AiController()
{
    // A controller destroyed mid-run must not free its members under the
    // worker: ask it to stop, then join. Cancelling the scheduler job is a
    // no-op before the first run (`runId_` is 0 and never minted).
    cancelRequested_.store(true);
    scheduler_.cancel(runId_);
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

void AiController::setClipId(const QString &clipId)
{
    if (clipId_ == clipId) {
        return;
    }
    clipId_ = clipId;
    emit clipIdChanged();
}

const genesis::project::Clip *AiController::clip() const
{
    if (editor_ == nullptr || clipId_.isEmpty()) {
        return nullptr;
    }
    return editor_->project().active().clip(clipId_.toStdString());
}

void AiController::refuse(const QString &reason)
{
    setError(reason);
    setState(QStringLiteral("Failed"));
    emit finished(false, reason);
}

bool AiController::modelPresent() const
{
    if (model_root_.empty()) {
        return false;
    }
    // A cheap existence check (not the full digest verify): the authoritative
    // SHA-256 check runs inside ai::install on the worker, off the UI thread.
    std::error_code ec;
    return std::filesystem::is_regular_file(ai::model_path(model_root_, runEntry_), ec);
}

void AiController::generateCaptions()
{
    if (running_) {
        return;
    }
    if (!available()) {
        refuse(QStringLiteral("disabled: whisper.cpp runtime not compiled in "
                              "(rebuild with GENESIS_AI_CAPTIONS=ON)"));
        return;
    }

    const genesis::project::Clip *selected = clip();
    if (selected == nullptr) {
        refuse(clipId_.isEmpty() ? QStringLiteral("No clip is selected.")
                                 : QStringLiteral("The selected clip no longer exists."));
        return;
    }
    const genesis::project::MediaItem *media = editor_->project().media_by_id(selected->media_id);
    if (media == nullptr) {
        refuse(QStringLiteral("That media is no longer in the bin."));
        return;
    }
    if (!media->has_audio) {
        refuse(QStringLiteral("The selected clip has no audio."));
        return;
    }

    // The pinned whisper entry: the one caption model the catalogue knows how
    // to obtain and run.
    const ai::ModelEntry *entry = nullptr;
    for (const ai::ModelEntry &row : ai::model_catalogue()) {
        if (row.id == "whisper") {
            entry = &row;
            break;
        }
    }
    if (entry == nullptr) {
        refuse(QStringLiteral("No caption model is available."));
        return;
    }

    // Fix the run's parameters now, so the worker and the apply read the same
    // clip position even if the clip moves mid-run (see the header note).
    runEntry_ = *entry;
    runClipStart_ = selected->start;
    runMediaPath_ = media->path;
    runStreamIndex_ =
            static_cast<std::uint32_t>(media->audio_track_position(selected->audio_stream));

    if (!modelPresent()) {
        // First run: the model is not on disk, so ask before downloading it.
        consentName_ = QString::fromStdString(runEntry_.id + " " + runEntry_.version);
        consentSize_ = human_size(runEntry_.size);
        consentLicense_ = QString::fromStdString(runEntry_.license);
        consentUrl_ = QString::fromStdString(runEntry_.url);
        setError(QString());
        setState(QStringLiteral("Consent"));
        emit consentChanged();
        return;
    }
    startRun();
}

void AiController::confirmDownload()
{
    if (state_ != QStringLiteral("Consent")) {
        return;
    }
    startRun();
}

void AiController::declineDownload()
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

void AiController::cancel()
{
    if (!running_) {
        return;
    }
    // Flag the job on the scheduler (cancel wins over the result when the
    // worker completes) and set the atomic the worker polls. `running` stays
    // true until finishCaptions lands, so the pane keeps showing progress
    // while the worker confirms the stop.
    scheduler_.cancel(runId_);
    cancelRequested_.store(true, std::memory_order_relaxed);
}

void AiController::startRun()
{
    // Queue the run on the scheduler, then start it. With max_concurrent = 1
    // and the `running_` guard holding off a second submit, this is the one
    // Running job. The scheduler lives on this (the controller's) thread; the
    // worker below never touches it, only the queued handlers do.
    runId_ = scheduler_.submit(jobs::JobRequest{ jobs::TranscribeRequest{ } });
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

    // R4: downloading, decoding and transcribing block, so they must not run
    // on the UI thread. The worker owns the resolved parameters and the seams
    // by value and reaches back into the controller only through the
    // cancellation flag and queued invokes, so no controller member is touched
    // off-thread. The thread is joined in finishCaptions, so it is never
    // reassigned while still running. The default fetcher/extractor/transcriber
    // are built here, on the worker, so a QNetworkAccessManager and any engine
    // state are created on the thread that uses them.
    workerThread_ = std::thread([this, root = model_root_, entry = runEntry_,
                                 media_path = runMediaPath_, stream_index = runStreamIndex_,
                                 clip_start = runClipStart_, fetcher = fetcher_,
                                 extractor = extractor_, transcriber = transcriber_,
                                 installer = installer_]() {
        workerThreadId_.store(reinterpret_cast<quintptr>(QThread::currentThread()));

        const ai::Fetcher fetch = fetcher ? fetcher : ai::Fetcher{ QtFetcher(&cancelRequested_) };
        const Extractor extract =
                extractor ? extractor : Extractor{ [](const ges::AudioExtractRequest &request) {
                    return ges::extract_audio(request);
                } };
        const Transcriber transcribe = transcriber
                ? transcriber
                : Transcriber{ [](std::string_view model, std::string_view pcm,
                                  const ai::TranscribeOptions &options,
                                  const ai::ProgressFn &progress, const ai::AbortFn &abort) {
                      return ai::WhisperProvider{ }.transcribe_file(model, pcm, options, progress,
                                                                    abort);
                  } };
        const Installer install = installer
                ? installer
                : Installer{ [](const std::filesystem::path &root, const ai::ModelEntry &entry,
                                const ai::Fetcher &fetch, const ai::Progress &progress) {
                      return ai::install(root, entry, fetch, progress);
                  } };

        // Stage 1: install the model (downloading it when absent). The
        // digest verify runs inside ai::install, off the UI thread.
        const ai::InstallResult installed =
                install(root, entry, fetch, [this](std::uintmax_t bytes, std::uintmax_t total) {
                    const double fraction = total > 0
                            ? std::clamp(static_cast<double>(bytes) / static_cast<double>(total),
                                         0.0, 1.0)
                            : 0.0;
                    reportProgress(0.4 * fraction, QStringLiteral("downloading model"));
                });
        if (!installed.ok()) {
            const bool cancelled = cancelRequested_.load(std::memory_order_relaxed);
            const std::string reason = installed.problems.empty() ? "the model download failed"
                                                                  : installed.problems.front();
            postFinish(false, cancelled ? "cancelled" : reason, { }, clip_start);
            return;
        }

        // Stage 2: decode the clip's whole audio stream to 16 kHz mono f32
        // PCM, the exact format the caption runtime reads.
        reportProgress(0.45, QStringLiteral("extracting audio"));
        const std::filesystem::path pcm =
                std::filesystem::temp_directory_path() / "genesis-captions.pcm";
        std::error_code ec;
        std::filesystem::remove(pcm, ec);
        const ges::AudioExtractResult extracted =
                extract(ges::AudioExtractRequest{ media_path, pcm, stream_index });
        if (extracted.error != ges::AudioExtractError::None) {
            std::filesystem::remove(pcm, ec);
            const bool cancelled = cancelRequested_.load(std::memory_order_relaxed);
            postFinish(false, cancelled ? "cancelled" : extracted.message, { }, clip_start);
            return;
        }
        reportProgress(0.5, QStringLiteral("extracting audio"));

        // Stage 3: transcribe the PCM into timed segments.
        const ai::TranscribeOutcome outcome = transcribe(
                installed.installed->string(), pcm.string(), ai::TranscribeOptions{ },
                [this](double fraction, std::string_view stage) {
                    reportProgress(
                            0.5 + 0.5 * std::clamp(fraction, 0.0, 1.0),
                            QString::fromUtf8(stage.data(), static_cast<qsizetype>(stage.size())));
                },
                [this]() { return cancelRequested_.load(std::memory_order_relaxed); });
        std::filesystem::remove(pcm, ec);

        if (!outcome.ok) {
            const bool cancelled = cancelRequested_.load(std::memory_order_relaxed)
                    || outcome.error == "cancelled";
            postFinish(false, cancelled ? "cancelled" : outcome.error, { }, clip_start);
            return;
        }
        postFinish(true, { }, outcome.transcript.segments, clip_start);
    });
}

void AiController::reportProgress(double fraction, const QString &stage)
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
                    // The scheduler owns the run's progress (clamped to 0..1); the
                    // properties mirror it for the pane's bar.
                    scheduler_.report_progress(runId_,
                                               jobs::Progress{ fraction, stage.toStdString() });
                    setProgress(fraction);
                    setStage(stage);
                },
                Qt::QueuedConnection);
    }
}

void AiController::postFinish(bool ok, const std::string &error,
                              const std::vector<jobs::CaptionSegment> &segments,
                              const Rational &clipStart)
{
    // The worker is returning; hand the outcome to the controller's thread so
    // the segments and the scheduler transition stay on the one thread that
    // owns them.
    QMetaObject::invokeMethod(
            this,
            [this, ok, error, segments, clipStart]() {
                finishCaptions(ok, QString::fromStdString(error), segments, clipStart);
            },
            Qt::QueuedConnection);
}

void AiController::finishCaptions(bool ok, const QString &error,
                                  const std::vector<jobs::CaptionSegment> &segments,
                                  const Rational &clipStart)
{
    // Runs on the controller's thread. Drive the scheduler to its terminal
    // state, fold the outcome into the properties and emit the signals, then
    // join the worker - which has already posted this call and is returning -
    // so the next run can safely reuse workerThread_.
    bool final_ok = ok;
    QString final_error = error;

    if (ok) {
        QString refusal;
        if (applyCaptions(segments, clipStart, refusal)) {
            scheduler_.complete(runId_, jobs::JobResult{ jobs::TranscriptResult{ segments } });
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
    } else if (final_error == QStringLiteral("cancelled")) {
        // cancel() already flagged the job, so completing it now lands it
        // Cancelled (cancel wins, the result is discarded).
        scheduler_.complete(runId_, jobs::JobResult{ });
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

bool AiController::applyCaptions(const std::vector<jobs::CaptionSegment> &segments,
                                 const Rational &clipStart, QString &refusal)
{
    // One AddTextClip per segment, placed above the video, wrapped in a single
    // Batch so the whole caption run is one undo step. Empty text and
    // degenerate spans are dropped.
    genesis::project::Batch batch;
    batch.commands.reserve(segments.size());
    for (const jobs::CaptionSegment &segment : segments) {
        if (segment.text.empty() || segment.end <= segment.start) {
            continue;
        }
        genesis::project::AddTextClip clip;
        clip.above = true;
        clip.start = clipStart + seconds(segment.start);
        clip.duration = seconds(segment.end - segment.start);
        genesis::project::TextStyle style;
        style.content = segment.text;
        clip.style = std::move(style);
        batch.commands.push_back(genesis::project::Command{ std::move(clip) });
    }
    if (batch.commands.empty()) {
        // A transcript of silence: nothing to place, but not a failure.
        return true;
    }
    if (editor_ == nullptr) {
        refusal = QStringLiteral("no project");
        return false;
    }
    const auto result = editor_->apply(genesis::project::Command{ std::move(batch) });
    if (!result) {
        refusal = QString::fromStdString(std::string(result.error().message()));
        return false;
    }
    // A tolerated no-op (nothing changed) still succeeds: no undo step, no
    // failure. A real apply notifies the shell to refresh and the toolbar to
    // re-read its undo/redo/dirty state.
    emit historyChanged();
    emit projectEdited();
    return true;
}

void AiController::setState(const QString &value)
{
    if (state_ == value) {
        return;
    }
    state_ = value;
    emit stateChanged();
}

void AiController::setRunning(bool value)
{
    if (running_ == value) {
        return;
    }
    running_ = value;
    emit runningChanged();
}

void AiController::setProgress(double value)
{
    if (progress_ == value) {
        return;
    }
    progress_ = value;
    emit progressChanged();
}

void AiController::setStage(const QString &value)
{
    if (stage_ == value) {
        return;
    }
    stage_ = value;
    emit stageChanged();
}

void AiController::setError(const QString &value)
{
    if (error_ == value) {
        return;
    }
    error_ = value;
    emit errorChanged();
}
