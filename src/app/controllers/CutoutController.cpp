// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/controllers/CutoutController.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <QMetaObject>
#include <QString>
#include <QStringList>
#include <QThread>

#include "adapters/engine/ges/media/VideoFrames.h"
#include "ai/vision/BrushProvider.h"
#include "app/net/QtFetcher.h"
#include "project/Editor.h"
#include "project/command/Command.h"
#include "project/command/PropertyCommands.h"
#include "project/model/Media.h"

namespace ai = genesis::ai;
namespace vision = genesis::ai::vision;
namespace ges = genesis::adapters::engine::ges;
namespace jobs = genesis::jobs;

namespace {

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

// The subject a pane names, or null when it names none of the two real models.
std::optional<genesis::project::Subject> parse_subject(const QString &text)
{
    if (text == QStringLiteral("person")) {
        return genesis::project::Subject::Person;
    }
    if (text == QStringLiteral("object")) {
        return genesis::project::Subject::Object;
    }
    return std::nullopt;
}

// The media's frame rate as an exact fraction, from its probe: the exact
// fraction first, then the display double, then 30/1 (the same fallback the
// enhance controller uses - a source instant maps to the source frame counter,
// and the codebase assumes media fps == timeline fps).
void fps_from_media(const genesis::project::MediaItem &media, std::uint32_t &num,
                    std::uint32_t &den)
{
    num = 30;
    den = 1;
    const auto from_rational = [&](const genesis::core::Rational &rate) {
        if (rate.numerator() > 0 && rate.denominator() > 0) {
            num = static_cast<std::uint32_t>(rate.numerator());
            den = static_cast<std::uint32_t>(rate.denominator());
            return true;
        }
        return false;
    };
    if (media.frame_rate_fraction) {
        const auto parsed = genesis::core::Rational::parse(*media.frame_rate_fraction);
        if (parsed && from_rational(*parsed)) {
            return;
        }
    }
    if (media.frame_rate && *media.frame_rate > 0.0) {
        const auto approx = genesis::core::Rational::approximate(*media.frame_rate);
        if (approx && from_rational(*approx)) {
            return;
        }
    }
}

// The cutout's smart strokes as driver strokes, each named by the source frame
// its painted instant maps to. Only the smart tools ride (the plain discs are
// not a model refinement); a smart stroke with no recorded instant cannot name
// a frame to read, so it is skipped - it refines nothing until re-painted.
std::vector<vision::BrushStroke> smart_strokes(const genesis::project::Cutout &cutout,
                                               std::uint32_t fps_num, std::uint32_t fps_den)
{
    std::vector<vision::BrushStroke> out;
    if (fps_num == 0 || fps_den == 0) {
        return out;
    }
    const genesis::core::FrameRate rate{ genesis::core::Rational{
            static_cast<std::int64_t>(fps_num), static_cast<std::int64_t>(fps_den) } };
    for (const genesis::project::Stroke &stroke : cutout.strokes) {
        if (!stroke.is_smart() || !stroke.at) {
            continue;
        }
        const std::int64_t frame = rate.frame_at(*stroke.at);
        if (frame < 0) {
            continue;
        }
        vision::BrushStroke driver;
        driver.frame = static_cast<std::uint32_t>(frame);
        driver.tool = stroke.tool;
        driver.points = stroke.points;
        out.push_back(std::move(driver));
    }
    return out;
}

// The pinned catalogue entry with this id, or null when none matches.
const ai::ModelEntry *find_entry(std::string_view id)
{
    for (const ai::ModelEntry &row : ai::model_catalogue()) {
        if (row.id == id) {
            return &row;
        }
    }
    return nullptr;
}

} // namespace

CutoutController::CutoutController(genesis::project::Editor *editor, QObject *parent)
    : QObject(parent), editor_(editor), strokeDebounce_(this)
{
    // A burst of stroke commits coalesces into one regeneration: each commit
    // restarts the single-shot timer, so the re-mask fires once the painting
    // pauses rather than once per committed stroke.
    strokeDebounce_.setSingleShot(true);
    strokeDebounce_.setInterval(300);
    QObject::connect(&strokeDebounce_, &QTimer::timeout, this, [this]() {
        const QString id = pendingClipId_;
        pendingClipId_.clear();
        if (!id.isEmpty()) {
            regenerateClip(id);
        }
    });
}

CutoutController::~CutoutController()
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

void CutoutController::setClipId(const QString &clipId)
{
    if (clipId_ == clipId) {
        return;
    }
    clipId_ = clipId;
    emit clipIdChanged();
}

const genesis::project::Clip *CutoutController::clip() const
{
    if (editor_ == nullptr || clipId_.isEmpty()) {
        return nullptr;
    }
    return editor_->project().active().clip(clipId_.toStdString());
}

const genesis::project::Clip *CutoutController::clipById(const std::string &id) const
{
    if (editor_ == nullptr) {
        return nullptr;
    }
    return editor_->project().active().clip(id);
}

void CutoutController::refuse(const QString &reason)
{
    setError(reason);
    setState(QStringLiteral("Failed"));
    emit finished(false, reason);
}

std::vector<ai::ModelEntry> CutoutController::installList() const
{
    std::vector<ai::ModelEntry> list{ runEntry_ };
    if (runBrushEncoder_) {
        list.push_back(*runBrushEncoder_);
    }
    if (runBrushDecoder_) {
        list.push_back(*runBrushDecoder_);
    }
    return list;
}

void CutoutController::setConsentFor(const std::vector<ai::ModelEntry> &entries)
{
    QStringList names;
    QStringList licences;
    QStringList urls;
    std::uintmax_t total = 0;
    for (const ai::ModelEntry &entry : entries) {
        names << QString::fromStdString(entry.id + " " + entry.version);
        if (!entry.license.empty()) {
            licences << QString::fromStdString(entry.license);
        }
        if (!entry.url.empty()) {
            urls << QString::fromStdString(entry.url);
        }
        total += entry.size;
    }
    consentName_ = names.join(QStringLiteral(", "));
    consentSize_ = human_size(total);
    consentLicense_ = licences.join(QStringLiteral(", "));
    consentUrl_ = urls.join(QStringLiteral(", "));
}

bool CutoutController::modelPresent() const
{
    if (model_root_.empty()) {
        return false;
    }
    // A cheap existence check (not the full digest verify): the authoritative
    // SHA-256 check runs inside ai::install on the worker, off the UI thread.
    for (const ai::ModelEntry &entry : installList()) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(ai::model_path(model_root_, entry), ec)) {
            return false;
        }
    }
    return true;
}

void CutoutController::applyCutout(const QString &subject)
{
    if (running_) {
        return;
    }
    if (!available()) {
        refuse(QStringLiteral("disabled: ONNX Runtime not compiled in "
                              "(rebuild with GENESIS_AI_VISION=ON)"));
        return;
    }

    const std::optional<genesis::project::Subject> parsed = parse_subject(subject);
    if (!parsed) {
        refuse(QStringLiteral("That subject has no cutout model."));
        return;
    }
    const genesis::project::Subject chosen = *parsed;

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
    if (media->kind == genesis::project::MediaKind::Audio) {
        refuse(QStringLiteral("The selected clip has no picture."));
        return;
    }

    // The cutout the run will produce. mask_target() is the same single source
    // of truth the render path reads, so the model id and jobs::Subject the
    // driver keys masks with are exactly what flatten looks up later.
    const genesis::project::Cutout cutout{
        genesis::project::CutoutMode::Auto, chosen, genesis::project::DEFAULT_FEATHER, { }
    };
    const vision::MaskTarget target = vision::mask_target(cutout);

    // The pinned model entry the catalogue names for this subject's model.
    const ai::ModelEntry *entry = find_entry(target.model_id);
    if (entry == nullptr) {
        refuse(QStringLiteral("No cutout model is available."));
        return;
    }

    // Fix the run's parameters now, so the worker and the apply read the same
    // clip even if selection moves mid-run (see the header note).
    runEntry_ = *entry;
    runSubject_ = chosen;
    runClipId_ = selected->id;
    runMediaPath_ = media->path;
    // The byte size the render path folds into every mask key (the same
    // file_size the flatten spec reads), so a mask cut now is found later.
    std::error_code ec;
    runSourceSize_ = std::filesystem::file_size(std::filesystem::path(media->path), ec);
    if (ec) {
        runSourceSize_ = 0;
    }
    runMaskRoot_ = mask_root_;
    // An automatic cutout carries no strokes and no revision, and its success
    // applies the cutout command.
    runRevision_.clear();
    runStrokes_.clear();
    runBrushEncoder_.reset();
    runBrushDecoder_.reset();
    runApply_ = true;

    if (!modelPresent()) {
        // First run: the model is not on disk, so ask before downloading it.
        setConsentFor(installList());
        setError(QString());
        setState(QStringLiteral("Consent"));
        emit consentChanged();
        return;
    }
    startRun();
}

void CutoutController::clearCutout()
{
    if (running_) {
        return;
    }
    const genesis::project::Clip *selected = clip();
    if (selected == nullptr) {
        refuse(clipId_.isEmpty() ? QStringLiteral("No clip is selected.")
                                 : QStringLiteral("The selected clip no longer exists."));
        return;
    }
    if (editor_ == nullptr) {
        refuse(QStringLiteral("no project"));
        return;
    }
    // Removing a cutout needs no model and no worker: one SetClipCutout with no
    // cutout, one undo step, exactly like an inspector toggle.
    genesis::project::SetClipCutout clear;
    clear.clip_id = selected->id;
    clear.cutout = std::nullopt;
    const auto result = editor_->apply(genesis::project::Command{ std::move(clear) });
    if (!result) {
        refuse(QString::fromStdString(std::string(result.error().message())));
        return;
    }
    emit historyChanged();
    emit projectEdited();
}

void CutoutController::scheduleRegenerate(const QString &clipId)
{
    if (clipId.isEmpty()) {
        return;
    }
    pendingClipId_ = clipId;
    strokeDebounce_.start();
}

void CutoutController::regenerateClip(const QString &clipId)
{
    if (running_) {
        return;
    }
    const std::string id = clipId.toStdString();
    const genesis::project::Clip *selected = clipById(id);
    if (selected == nullptr) {
        refuse(QStringLiteral("The clip no longer exists."));
        return;
    }
    if (!selected->cutout) {
        refuse(QStringLiteral("The clip has no cutout to regenerate."));
        return;
    }
    const genesis::project::Cutout &cutout = *selected->cutout;
    // A cutout's subject names the model that masks it; Auto has no model (the
    // "auto" placeholder), so a custom cutout painted before a subject was
    // chosen cannot be regenerated until applyCutout names one.
    if (cutout.subject == genesis::project::Subject::Auto) {
        refuse(QStringLiteral("Apply a cutout subject first - the smart brush refines a "
                              "subject's mask."));
        return;
    }
    const genesis::project::Subject subject = cutout.subject;

    const genesis::project::MediaItem *media =
            editor_ == nullptr ? nullptr : editor_->project().media_by_id(selected->media_id);
    if (media == nullptr) {
        refuse(QStringLiteral("That media is no longer in the bin."));
        return;
    }
    if (media->kind == genesis::project::MediaKind::Audio) {
        refuse(QStringLiteral("The selected clip has no picture."));
        return;
    }
    if (!available()) {
        refuse(QStringLiteral("disabled: ONNX Runtime not compiled in "
                              "(rebuild with GENESIS_AI_VISION=ON)"));
        return;
    }

    const vision::MaskTarget target = vision::mask_target(cutout);
    const ai::ModelEntry *entry = find_entry(target.model_id);
    if (entry == nullptr) {
        refuse(QStringLiteral("No cutout model is available."));
        return;
    }

    // The source frame rate a stroke's painted instant maps to: the media's
    // probe, matching the frame counter the frame source delivers.
    std::uint32_t fps_num = 30;
    std::uint32_t fps_den = 1;
    fps_from_media(*media, fps_num, fps_den);

    // Fix the run's parameters now. The cutout is already on the clip, so a
    // regenerate adds no undo step - it only re-keys and refines masks.
    runEntry_ = *entry;
    runSubject_ = subject;
    runClipId_ = selected->id;
    runMediaPath_ = media->path;
    std::error_code ec;
    runSourceSize_ = std::filesystem::file_size(std::filesystem::path(media->path), ec);
    if (ec) {
        runSourceSize_ = 0;
    }
    runMaskRoot_ = mask_root_;
    runRevision_ = vision::mask_revision(cutout);
    runStrokes_ = smart_strokes(cutout, fps_num, fps_den);
    runApply_ = false;

    // The smart-brush models ride only when there are smart strokes to refine
    // and the runtime can run them.
    runBrushEncoder_.reset();
    runBrushDecoder_.reset();
    if (!runStrokes_.empty() && vision::BrushProvider::available()) {
        const ai::ModelEntry *encoder = find_entry("slimsam-encoder");
        const ai::ModelEntry *decoder = find_entry("slimsam-decoder");
        if (encoder != nullptr && decoder != nullptr) {
            runBrushEncoder_ = *encoder;
            runBrushDecoder_ = *decoder;
        }
    }

    if (!modelPresent()) {
        setConsentFor(installList());
        setError(QString());
        setState(QStringLiteral("Consent"));
        emit consentChanged();
        return;
    }
    startRun();
}

void CutoutController::confirmDownload()
{
    if (state_ != QStringLiteral("Consent")) {
        return;
    }
    startRun();
}

void CutoutController::declineDownload()
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

void CutoutController::cancel()
{
    if (!running_) {
        return;
    }
    // Flag the job on the scheduler (cancel wins over the result when the
    // worker completes) and set the atomic the worker polls. `running` stays
    // true until finishCutout lands, so the pane keeps showing progress while
    // the worker confirms the stop.
    scheduler_.cancel(runId_);
    cancelRequested_.store(true, std::memory_order_relaxed);
}

void CutoutController::startRun()
{
    // Queue the run on the scheduler, then start it. With max_concurrent = 1
    // and the `running_` guard holding off a second submit, this is the one
    // Running job. The scheduler lives on this (the controller's) thread; the
    // worker below never touches it, only the queued handlers do.
    runId_ = scheduler_.submit(jobs::JobRequest{ jobs::DetectSubjectsRequest{ } });
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

    // R4: downloading, decoding and segmenting block, so they must not run on
    // the UI thread. The worker owns the resolved parameters and the seams by
    // value and reaches back into the controller only through the cancellation
    // flag and queued invokes, so no controller member is touched off-thread.
    // The thread is joined in finishCutout, so it is never reassigned while
    // still running. The default fetcher/installer/frame-source/segmenter are
    // built here, on the worker, so a QNetworkAccessManager and any engine
    // state are created on the thread that uses them.
    workerThread_ = std::thread([this, root = model_root_, install_list = installList(),
                                 subject = runSubject_, media_path = runMediaPath_,
                                 source_size = runSourceSize_, mask_root = runMaskRoot_,
                                 revision = runRevision_, strokes = runStrokes_, fetcher = fetcher_,
                                 installer = installer_, frame_source = frameSource_,
                                 segment_fn = segmentFn_, brush_refine_fn = brushRefineFn_]() {
        workerThreadId_.store(reinterpret_cast<quintptr>(QThread::currentThread()));

        const ai::Fetcher fetch = fetcher ? fetcher : ai::Fetcher{ QtFetcher(&cancelRequested_) };
        const Installer install = installer
                ? installer
                : Installer{ [](const std::filesystem::path &root, const ai::ModelEntry &entry,
                                const ai::Fetcher &fetch, const ai::Progress &progress) {
                      return ai::install(root, entry, fetch, progress);
                  } };

        // Stage 1: install every model this run needs (reusing any already
        // on disk). The digest verify runs inside ai::install, off the UI
        // thread. The segmentation model is first; the smart-brush
        // encoder/decoder follow when the run refines smart strokes.
        std::vector<std::filesystem::path> installed_paths;
        installed_paths.reserve(install_list.size());
        for (const ai::ModelEntry &model : install_list) {
            const ai::InstallResult installed =
                    install(root, model, fetch, [this](std::uintmax_t bytes, std::uintmax_t total) {
                        const double fraction = total > 0
                                ? std::clamp(static_cast<double>(bytes)
                                                     / static_cast<double>(total),
                                             0.0, 1.0)
                                : 0.0;
                        reportProgress(0.4 * fraction, QStringLiteral("downloading model"));
                    });
            if (!installed.ok()) {
                const bool cancelled = cancelRequested_.load(std::memory_order_relaxed);
                const std::string reason = installed.problems.empty() ? "the model download failed"
                                                                      : installed.problems.front();
                postFinish(false, cancelled ? "cancelled" : reason);
                return;
            }
            installed_paths.push_back(*installed.installed);
        }

        // The segment seam: one frame in, one alpha mask out. The default
        // loads the SegmentationProvider once (reusing its session across
        // every frame) and reaches its segment(); tests inject a fake so no
        // model and no runtime load.
        SegmentFn segment;
        if (segment_fn) {
            segment = segment_fn;
        } else {
            auto provider = std::make_shared<vision::SegmentationProvider>();
            const vision::SegmentModel model = subject == genesis::project::Subject::Person
                    ? vision::SegmentModel::Rvm
                    : vision::SegmentModel::IsNet;
            segment = SegmentFn{ [provider, model, path = installed_paths[0].string()](
                                         const vision::FrameView &frame,
                                         const vision::ProgressFn &progress,
                                         const vision::AbortFn &abort) mutable {
                if (!provider->ok()) {
                    *provider = vision::SegmentationProvider::load(path, model);
                    if (!provider->ok()) {
                        vision::SegmentationOutcome out;
                        out.error = provider->error();
                        return out;
                    }
                }
                return provider->segment(frame, progress, abort);
            } };
        }

        // The frame source: decodes the clip's whole video stream to RGB
        // frames. The default reaches ges::extract_video_frames; tests
        // inject a fake so no GStreamer decode runs. Every source frame is
        // delivered (every_nth = 1), matching the render path's stride-1
        // MaskSpec so a mask cut now is found later.
        FrameSource decode;
        if (frame_source) {
            decode = frame_source;
        } else {
            decode = FrameSource{ [this, media_path](const vision::FrameSink &sink) {
                const ges::VideoFramesResult r = ges::extract_video_frames(
                        ges::VideoFramesRequest{ std::filesystem::path(media_path), 0, 1, 0.0 },
                        [&sink](const vision::FrameView &frame, std::uint32_t index) {
                            return sink(frame, index);
                        },
                        [this]() { return cancelRequested_.load(std::memory_order_relaxed); });
                vision::FrameSourceOutcome out;
                out.ok = r.error == ges::VideoFramesError::None;
                out.error = r.message;
                return out;
            } };
        }

        // The smart-brush refine seam: an injected fake (tests) wins;
        // otherwise, with smart strokes and the encoder/decoder installed,
        // load one BrushProvider and refine each frame's base alpha in
        // place. A refine failure keeps the base alpha, so it is non-fatal.
        vision::BrushRefineFn brush_refine;
        if (brush_refine_fn) {
            brush_refine = brush_refine_fn;
        } else if (!strokes.empty() && installed_paths.size() >= 3) {
            auto provider = std::make_shared<vision::BrushProvider>(vision::BrushProvider::load(
                    installed_paths[1].string(), installed_paths[2].string()));
            brush_refine = vision::BrushRefineFn{
                [provider](std::vector<float> &alpha, const vision::FrameView &frame,
                           const std::vector<vision::BrushStroke> &fs,
                           std::string &error) { return provider->refine(alpha, frame, fs, error); }
            };
        }

        // The cutout the masks are keyed and applied under: the same
        // mask_target the render path derives after the command lands.
        const vision::MaskTarget target =
                vision::mask_target(genesis::project::Cutout{ genesis::project::CutoutMode::Auto,
                                                              subject,
                                                              genesis::project::DEFAULT_FEATHER,
                                                              { } });

        // Stage 2: decode -> segment -> (refine) -> store, reusing any mask
        // already fresh on disk. The revision and strokes make a
        // re-painted cutout key and refine under its new stroke set rather
        // than reusing a stale base mask.
        vision::MaskGenerationRequest request;
        request.source = std::filesystem::path(media_path);
        request.source_size = source_size;
        request.subject = target.subject;
        request.model_id = target.model_id;
        request.mask_root = mask_root;
        request.revision = revision;
        request.strokes = strokes;

        const vision::MaskGenerationResult result = vision::generate_masks(
                request, decode, segment, vision::SegmentationProvider::available(),
                [this](double fraction, std::string_view stage) {
                    reportProgress(
                            0.4 + 0.6 * std::clamp(fraction, 0.0, 1.0),
                            QString::fromUtf8(stage.data(), static_cast<qsizetype>(stage.size())));
                },
                [this]() { return cancelRequested_.load(std::memory_order_relaxed); },
                brush_refine);

        if (result.error == vision::MaskGenerationError::None) {
            postFinish(true, { });
        } else if (result.error == vision::MaskGenerationError::Cancelled) {
            postFinish(false, "cancelled");
        } else {
            postFinish(false, result.message.empty() ? "mask generation failed" : result.message);
        }
    });
}

void CutoutController::reportProgress(double fraction, const QString &stage)
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

void CutoutController::postFinish(bool ok, const std::string &error)
{
    // The worker is returning; hand the outcome to the controller's thread so
    // the scheduler transition and the apply stay on the one thread that owns
    // them.
    QMetaObject::invokeMethod(
            this, [this, ok, error]() { finishCutout(ok, QString::fromStdString(error)); },
            Qt::QueuedConnection);
}

void CutoutController::finishCutout(bool ok, const QString &error)
{
    // Runs on the controller's thread. Drive the scheduler to its terminal
    // state, fold the outcome into the properties and emit the signals, then
    // join the worker - which has already posted this call and is returning -
    // so the next run can safely reuse workerThread_.
    bool final_ok = ok;
    QString final_error = error;

    if (ok) {
        if (runApply_) {
            QString refusal;
            if (applyCutoutCommand(refusal)) {
                scheduler_.complete(runId_, jobs::JobResult{ jobs::SubjectsResult{ } });
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
            // A regenerate: the cutout is already on the clip (AddCutoutStroke
            // placed it), so the masks were re-keyed and refined in place. No
            // command runs, so no new undo step - only projectEdited fires.
            scheduler_.complete(runId_, jobs::JobResult{ jobs::SubjectsResult{ } });
            setProgress(1.0);
            setState(QStringLiteral("Done"));
            setError(QString());
            emit projectEdited();
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

bool CutoutController::applyCutoutCommand(QString &refusal)
{
    if (editor_ == nullptr) {
        refusal = QStringLiteral("no project");
        return false;
    }
    // One SetClipCutout: an automatic cutout for the resolved subject, one
    // undo step, and the render path picks the masks up automatically because
    // its mask_target derives the same keying the driver wrote them under.
    genesis::project::SetClipCutout apply;
    apply.clip_id = runClipId_;
    apply.cutout = genesis::project::Cutout{
        genesis::project::CutoutMode::Auto, runSubject_, genesis::project::DEFAULT_FEATHER, { }
    };
    const auto result = editor_->apply(genesis::project::Command{ std::move(apply) });
    if (!result) {
        refusal = QString::fromStdString(std::string(result.error().message()));
        return false;
    }
    emit historyChanged();
    emit projectEdited();
    return true;
}

void CutoutController::setState(const QString &value)
{
    if (state_ == value) {
        return;
    }
    state_ = value;
    emit stateChanged();
}

void CutoutController::setRunning(bool value)
{
    if (running_ == value) {
        return;
    }
    running_ = value;
    emit runningChanged();
}

void CutoutController::setProgress(double value)
{
    if (progress_ == value) {
        return;
    }
    progress_ = value;
    emit progressChanged();
}

void CutoutController::setStage(const QString &value)
{
    if (stage_ == value) {
        return;
    }
    stage_ = value;
    emit stageChanged();
}

void CutoutController::setError(const QString &value)
{
    if (error_ == value) {
        return;
    }
    error_ = value;
    emit errorChanged();
}
