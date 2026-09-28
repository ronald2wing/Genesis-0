// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

// GStreamer/glib pull in gdbusintrospection.h, whose `signals` field collides
// with the Qt `signals` keyword macro - include the glib headers first so the
// field is parsed before Qt defines the macro. EnhanceController.h includes
// <QObject>, so these must come before it.
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include "app/controllers/EnhanceController.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <QMetaObject>
#include <QString>
#include <QThread>

#include "adapters/engine/ges/media/VideoFrames.h"
#include "app/net/QtFetcher.h"
#include "project/Editor.h"
#include "project/command/Command.h"
#include "project/model/Media.h"

namespace ai = genesis::ai;
namespace vision = genesis::ai::vision;
namespace ges = genesis::adapters::engine::ges;
namespace jobs = genesis::jobs;

using genesis::project::MediaKind;
using genesis::project::MediaOrigin;
using genesis::project::NewMedia;
using genesis::project::ReplaceClipMedia;

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

// The largest even value at or below `v`, so VP8's 4:2:0 accepts the frame.
std::uint32_t even_down(std::uint32_t v)
{
    return v & ~1u;
}

// The output frame rate as an exact fraction, from the media's probe: the
// exact fraction first, then the display double, then 30/1.
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

// The single-slot handoff between the push-based frame source (the producer
// thread decodes and enhances) and the pull-based encoder (the worker thread
// pulls one frame at a time). One buffer is enough: the producer waits for the
// consumer to take a frame before writing the next, so memory stays bounded on
// an arbitrarily long clip. Both ends block on one condition variable and
// leave through `consumer_stopped`, so a cancelled encoder can always unblock
// a waiting producer and join cleanly.
struct FrameHandoff
{
    std::mutex mutex;
    std::condition_variable cond;
    bool has_frame = false;
    bool producer_done = false;
    bool consumer_stopped = false;
    std::string error;
    std::vector<std::uint8_t> buffer;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    FrameHandoff(std::uint32_t w, std::uint32_t h)
        : buffer(static_cast<std::size_t>(w) * h * 3), width(w), height(h)
    {
    }

    // Deposits one enhanced frame, cropped to width x height (drops the tail
    // row/column when a denoise-only pass left the source's odd size; a 2x/4x
    // frame is already even). Returns false when the consumer has stopped.
    bool produce(const std::vector<std::uint8_t> &rgb, std::uint32_t src_w)
    {
        std::unique_lock lock(mutex);
        cond.wait(lock, [this] { return !has_frame || consumer_stopped; });
        if (consumer_stopped) {
            return false;
        }
        const std::size_t row_bytes = static_cast<std::size_t>(width) * 3;
        const std::size_t src_row = static_cast<std::size_t>(src_w) * 3;
        for (std::uint32_t y = 0; y < height; ++y) {
            std::memcpy(buffer.data() + static_cast<std::size_t>(y) * row_bytes,
                        rgb.data() + static_cast<std::size_t>(y) * src_row, row_bytes);
        }
        has_frame = true;
        cond.notify_all();
        return true;
    }

    // Hands the next frame to the consumer. Returns false when the producer
    // finished, failed, was cancelled, or the consumer stopped.
    bool consume(vision::FrameView &frame, const std::function<bool()> &cancel)
    {
        std::unique_lock lock(mutex);
        cond.wait(lock, [this, &cancel] {
            return has_frame || producer_done || consumer_stopped || !error.empty() || cancel();
        });
        if (!has_frame) {
            return false;
        }
        frame.rgb = buffer.data();
        frame.width = width;
        frame.height = height;
        has_frame = false;
        cond.notify_all();
        return true;
    }

    // Parks the producer's failure; the first one wins, so a decode error is
    // not overwritten by the enhance error that follows it.
    void fail(std::string reason)
    {
        std::lock_guard lock(mutex);
        if (error.empty()) {
            error = std::move(reason);
        }
        cond.notify_all();
    }

    // Marks the producer's clean end (no more frames).
    void finish()
    {
        std::lock_guard lock(mutex);
        producer_done = true;
        cond.notify_all();
    }

    // Marks the consumer's stop: a producer blocked in produce() wakes and
    // returns false rather than waiting forever for a consumer that left.
    void stop()
    {
        std::lock_guard lock(mutex);
        consumer_stopped = true;
        cond.notify_all();
    }

    std::string take_error()
    {
        std::lock_guard lock(mutex);
        return error;
    }
};

// The provisional VP8/WebM encoder: appsrc -> videoconvert -> vp8enc ->
// webmmux -> filesink, pulling each enhanced frame through `next`. It is the
// app-side counterpart to the engine-side encode seam (owned by a parallel
// scope); the controller injects it by default and a test replaces it with a
// fake so no GStreamer encode runs in CI.
EnhanceController::EncodeOutcome encode_webm_vp8(const EnhanceController::EncodeRequest &request,
                                                 const EnhanceController::NextFrameFn &next,
                                                 const std::function<bool()> &cancel)
{
    EnhanceController::EncodeOutcome outcome;

    // Make the destination's directory and clear any stale file, so filesink
    // never fails on a missing parent and a failed run cannot leave a previous
    // encode behind.
    std::error_code ec;
    const std::filesystem::path parent = request.destination.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    std::filesystem::remove(request.destination, ec);

    GstElement *pipeline = gst_pipeline_new("enhance-encode");
    GstElement *src = gst_element_factory_make("appsrc", "src");
    GstElement *convert = gst_element_factory_make("videoconvert", "convert");
    GstElement *enc = gst_element_factory_make("vp8enc", "enc");
    GstElement *mux = gst_element_factory_make("webmmux", "mux");
    GstElement *sink = gst_element_factory_make("filesink", "sink");
    if (pipeline == nullptr || src == nullptr || convert == nullptr || enc == nullptr
        || mux == nullptr || sink == nullptr) {
        gst_clear_object(&pipeline);
        gst_clear_object(&src);
        gst_clear_object(&convert);
        gst_clear_object(&enc);
        gst_clear_object(&mux);
        gst_clear_object(&sink);
        outcome.error = "a pipeline element is missing from the GStreamer registry";
        return outcome;
    }

    // Pin the input to packed RGB8 at the fixed enhanced size and rate, so the
    // encoder sees exactly the frames the producer hands it.
    GstCaps *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGB", "width",
                                        G_TYPE_INT, static_cast<gint>(request.width), "height",
                                        G_TYPE_INT, static_cast<gint>(request.height), "framerate",
                                        GST_TYPE_FRACTION, static_cast<gint>(request.fps_num),
                                        static_cast<gint>(request.fps_den), nullptr);
    g_object_set(src, "caps", caps, "format", GST_FORMAT_TIME, nullptr);
    gst_caps_unref(caps);

    g_object_set(sink, "location", request.destination.string().c_str(), nullptr);

    gst_bin_add_many(GST_BIN(pipeline), src, convert, enc, mux, sink, nullptr);
    if (!gst_element_link_many(src, convert, enc, mux, sink, nullptr)) {
        gst_object_unref(pipeline);
        outcome.error = "cannot link the encode pipeline";
        return outcome;
    }

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        outcome.error = "the encode pipeline failed to start";
        return outcome;
    }

    // One frame's duration, from the exact rate.
    const GstClockTime frame_dur =
            gst_util_uint64_scale(GST_SECOND, request.fps_den, request.fps_num);

    std::uint64_t pushed = 0;
    GstClockTime pts = 0;
    bool cancelled = false;
    vision::FrameView frame;
    while (!cancel() && next(frame)) {
        const std::size_t size = static_cast<std::size_t>(frame.width) * frame.height * 3;
        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
        GstMapInfo map;
        if (gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
            std::memcpy(map.data, frame.rgb, size);
            gst_buffer_unmap(buffer, &map);
        }
        GST_BUFFER_PTS(buffer) = pts;
        GST_BUFFER_DURATION(buffer) = frame_dur;
        pts += frame_dur;
        ++pushed;
        if (gst_app_src_push_buffer(GST_APP_SRC(src), buffer) != GST_FLOW_OK) {
            outcome.error = "the encode pipeline rejected a frame";
            break;
        }
        if (cancel()) {
            cancelled = true;
            break;
        }
    }
    if (cancel()) {
        cancelled = true;
    }

    gst_app_src_end_of_stream(GST_APP_SRC(src));

    // Drive to EOS or error, bounded by a wall-clock timeout. A raw encode has
    // no live element and runs faster than real time; the bound only guards a
    // wedged pipeline.
    GstBus *bus = gst_element_get_bus(pipeline);
    bool clean_eos = false;
    std::string detail;
    for (;;) {
        GstMessage *message = gst_bus_timed_pop_filtered(
                bus, 10 * GST_SECOND,
                static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        if (message == nullptr) {
            detail = "the encode pipeline timed out";
            break;
        }
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
            clean_eos = true;
            gst_message_unref(message);
            break;
        }
        GError *error = nullptr;
        gchar *debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        if (error != nullptr) {
            detail = error->message;
        }
        g_clear_error(&error);
        g_free(debug);
        gst_message_unref(message);
        break;
    }
    gst_object_unref(bus);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    if (cancelled) {
        outcome.error = "cancelled";
        return outcome;
    }
    if (!clean_eos) {
        outcome.error = detail.empty() ? "encoding failed" : detail;
        return outcome;
    }
    if (pushed == 0) {
        outcome.error = "encoding produced no frames";
        return outcome;
    }
    outcome.ok = true;
    return outcome;
}

} // namespace

EnhanceController::EnhanceController(genesis::project::Editor *editor, QObject *parent)
    : QObject(parent), editor_(editor)
{
}

EnhanceController::~EnhanceController()
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

void EnhanceController::setClipId(const QString &clipId)
{
    if (clipId_ == clipId) {
        return;
    }
    clipId_ = clipId;
    emit clipIdChanged();
}

void EnhanceController::setFactor(int factor)
{
    // A negative ask (or a huge one) snaps into {1, 2, 4}: the provider runs
    // only those three factors, so anything else would be refused at enhance
    // time. Clamping here keeps the pane's chip in step with the run.
    const int clamped = factor >= 4 ? 4 : (factor >= 2 ? 2 : 1);
    if (factor_ == clamped) {
        return;
    }
    factor_ = clamped;
    emit factorChanged();
}

const genesis::project::Clip *EnhanceController::clip() const
{
    if (editor_ == nullptr || clipId_.isEmpty()) {
        return nullptr;
    }
    return editor_->project().active().clip(clipId_.toStdString());
}

void EnhanceController::refuse(const QString &reason)
{
    setError(reason);
    setState(QStringLiteral("Failed"));
    emit finished(false, reason);
}

bool EnhanceController::modelPresent() const
{
    if (model_root_.empty()) {
        return false;
    }
    // A cheap existence check (not the full digest verify): the authoritative
    // SHA-256 check runs inside ai::install on the worker, off the UI thread.
    std::error_code ec;
    return std::filesystem::is_regular_file(ai::model_path(model_root_, runEntry_), ec);
}

void EnhanceController::applyEnhance()
{
    if (running_) {
        return;
    }
    if (!available()) {
        refuse(QStringLiteral("disabled: ONNX Runtime not compiled in "
                              "(rebuild with GENESIS_AI_VISION=ON)"));
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
    if (media->kind == genesis::project::MediaKind::Audio) {
        refuse(QStringLiteral("The selected clip has no picture."));
        return;
    }
    if (!media->width || !media->height || *media->width == 0 || *media->height == 0) {
        refuse(QStringLiteral("That media has no picture to enhance."));
        return;
    }

    // The effective factor: the largest of {1, 2, 4} the ask allows that keeps
    // the longest output side at or under 4096 pixels.
    const std::uint32_t asked = factor_ >= 4 ? 4 : (factor_ >= 2 ? 2 : 1);
    const std::uint32_t effective = vision::enhance_factor(*media->width, *media->height, asked);

    // The pinned Real-ESRGAN model the catalogue names.
    const ai::ModelEntry *entry = nullptr;
    for (const ai::ModelEntry &row : ai::model_catalogue()) {
        if (row.id == "real-esrgan") {
            entry = &row;
            break;
        }
    }
    if (entry == nullptr) {
        refuse(QStringLiteral("No enhance model is available."));
        return;
    }

    // Output dimensions: source * factor, evened down so VP8's 4:2:0 accepts
    // them (the producer crops the tail row/column for a denoise-only pass).
    const std::uint32_t out_w = even_down(*media->width * effective);
    const std::uint32_t out_h = even_down(*media->height * effective);

    // The enhanced copy lands next to the source (or under the output root the
    // shell sets) as "<stem>-enhanced.webm".
    const std::filesystem::path source(media->path);
    const std::filesystem::path parent = output_root_.empty() ? source.parent_path() : output_root_;
    const std::filesystem::path output = parent / (source.stem().string() + "-enhanced.webm");

    std::uint32_t fps_num = 30;
    std::uint32_t fps_den = 1;
    fps_from_media(*media, fps_num, fps_den);

    // Fix the run's parameters now, so the worker and the apply read the same
    // clip even if selection moves mid-run (see the header note).
    runEntry_ = *entry;
    runClipId_ = selected->id;
    runMediaPath_ = media->path;
    runEncodeRequest_ = EncodeRequest{ output, out_w, out_h, fps_num, fps_den };
    runFactor_ = effective;
    runEstimatedFrames_ = [&]() {
        if (!media->duration) {
            return std::uint64_t{ 0 };
        }
        const double seconds = media->duration->as_double();
        const double rate =
                fps_den > 0 ? static_cast<double>(fps_num) / static_cast<double>(fps_den) : 30.0;
        return static_cast<std::uint64_t>(seconds * rate);
    }();

    // The NewMedia the apply re-points the clip at: a video-only VP8 copy,
    // shelved under Generated by its Enhanced origin.
    runOutput_ = NewMedia{ };
    runOutput_.path = output.string();
    runOutput_.name = output.filename().string();
    runOutput_.duration = media->duration;
    runOutput_.kind = MediaKind::Video;
    runOutput_.width = out_w;
    runOutput_.height = out_h;
    runOutput_.frame_rate =
            fps_den > 0 ? static_cast<double>(fps_num) / static_cast<double>(fps_den) : 30.0;
    runOutput_.frame_rate_fraction = std::to_string(fps_num) + "/" + std::to_string(fps_den);
    runOutput_.video_codec = "vp8";
    runOutput_.has_audio = false;
    runOutput_.origin = MediaOrigin::Enhanced;

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

void EnhanceController::confirmDownload()
{
    if (state_ != QStringLiteral("Consent")) {
        return;
    }
    startRun();
}

void EnhanceController::declineDownload()
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

void EnhanceController::cancel()
{
    if (!running_) {
        return;
    }
    // Flag the job on the scheduler (cancel wins over the result when the
    // worker completes) and set the atomic the worker polls. `running` stays
    // true until finishEnhance lands, so the pane keeps showing progress while
    // the worker confirms the stop.
    scheduler_.cancel(runId_);
    cancelRequested_.store(true, std::memory_order_relaxed);
}

void EnhanceController::startRun()
{
    // Queue the run on the scheduler, then start it. With max_concurrent = 1
    // and the `running_` guard holding off a second submit, this is the one
    // Running job. The scheduler lives on this (the controller's) thread; the
    // worker below never touches it, only the queued handlers do.
    runId_ = scheduler_.submit(
            jobs::JobRequest{ jobs::EnhanceRequest{ runMediaPath_, runFactor_ } });
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

    // Downloading, decoding, enhancing and encoding all block, so they must
    // not run on the UI thread. The worker owns the resolved parameters and
    // the seams by value and reaches back into the controller only through the
    // cancellation flag and queued invokes, so no controller member is touched
    // off-thread. The thread is joined in finishEnhance, so it is never
    // reassigned while still running. The default fetcher/installer/frame
    // source/enhancer/encoder are built here, on the worker, so a
    // QNetworkAccessManager and any engine state are created on the thread
    // that uses them.
    workerThread_ = std::thread([this, root = model_root_, entry = runEntry_,
                                 media_path = runMediaPath_, encode = runEncodeRequest_,
                                 factor = runFactor_, estimated = runEstimatedFrames_,
                                 fetcher = fetcher_, installer = installer_,
                                 frame_source = frameSource_, enhance_fn = enhanceFn_,
                                 encoder = encoder_]() {
        workerThreadId_.store(reinterpret_cast<quintptr>(QThread::currentThread()));

        const ai::Fetcher fetch = fetcher ? fetcher : ai::Fetcher{ QtFetcher(&cancelRequested_) };
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
                    reportProgress(0.2 * fraction, QStringLiteral("downloading model"));
                });
        if (!installed.ok()) {
            const bool cancelled = cancelRequested_.load(std::memory_order_relaxed);
            const std::string reason = installed.problems.empty() ? "the model download failed"
                                                                  : installed.problems.front();
            postFinish(false, cancelled ? "cancelled" : reason);
            return;
        }

        // The enhance seam: one frame and a factor in, one enhanced frame
        // out. The default loads the EnhanceProvider once and reuses its
        // session across every frame; tests inject a fake so no model and
        // no runtime load.
        EnhanceFn enhance;
        if (enhance_fn) {
            enhance = enhance_fn;
        } else {
            auto provider = std::make_shared<vision::EnhanceProvider>();
            enhance = EnhanceFn{ [provider, path = installed.installed->string()](
                                         const vision::FrameView &frame, std::uint32_t f,
                                         const vision::ProgressFn &progress,
                                         const vision::AbortFn &abort) mutable {
                if (!provider->ok()) {
                    *provider = vision::EnhanceProvider::load(path);
                    if (!provider->ok()) {
                        vision::EnhanceOutcome out;
                        out.error = provider->error();
                        return out;
                    }
                }
                return provider->enhance(frame, f, progress, abort);
            } };
        }

        // The frame source: decodes the clip's whole video stream to RGB
        // frames. The default reaches ges::extract_video_frames; tests
        // inject a fake so no GStreamer decode runs. Every source frame is
        // delivered (every_nth = 1), so the enhanced copy stands in for the
        // original frame for frame.
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

        // The encode seam: the default is the provisional GStreamer
        // VP8/WebM pipeline; tests inject a fake so no GStreamer runs.
        Encoder encode_frames = encoder ? encoder : Encoder{ encode_webm_vp8 };

        // The single-slot handoff between the push-based frame source and
        // the pull-based encoder, at the fixed enhanced size.
        FrameHandoff handoff(encode.width, encode.height);

        // The producer decodes and enhances on its own thread, depositing
        // each frame for the encoder to pull.
        std::thread producer([&]() {
            const vision::FrameSourceOutcome source = decode([&](const vision::FrameView &frame,
                                                                 std::uint32_t index) {
                if (cancelRequested_.load(std::memory_order_relaxed)) {
                    return false;
                }
                const double fraction = estimated > 0
                        ? std::min(1.0, static_cast<double>(index) / static_cast<double>(estimated))
                        : 0.0;
                reportProgress(0.2 + 0.8 * fraction, QStringLiteral("enhancing"));
                const vision::EnhanceOutcome enhanced = enhance(frame, factor, { }, [this]() {
                    return cancelRequested_.load(std::memory_order_relaxed);
                });
                if (!enhanced.ok) {
                    handoff.fail(enhanced.error.empty() ? "enhancement failed" : enhanced.error);
                    return false;
                }
                return handoff.produce(enhanced.rgb, enhanced.width);
            });
            if (!source.ok) {
                handoff.fail(source.error.empty() ? "frame decode failed" : source.error);
            }
            handoff.finish();
        });

        // The encoder pulls on this (the worker) thread; a frame that
        // arrives counts toward the run's proof of work.
        std::uint64_t encoded_frames = 0;
        EncodeOutcome enc_out = encode_frames(
                encode,
                [&](vision::FrameView &frame) {
                    const bool got = handoff.consume(frame, [this]() {
                        return cancelRequested_.load(std::memory_order_relaxed);
                    });
                    if (got) {
                        ++encoded_frames;
                    }
                    return got;
                },
                [this]() { return cancelRequested_.load(std::memory_order_relaxed); });

        // Unblock the producer (if it is still waiting for a free slot),
        // then join it before touching any shared state.
        handoff.stop();
        if (producer.joinable()) {
            producer.join();
        }

        // Resolve the run's outcome. The producer's own failure wins over
        // the encode outcome (a decode/enhance error stops the frames the
        // encoder would otherwise have written), then the encode outcome,
        // then an empty run. A partial file never survives a failure.
        const bool cancelled = cancelRequested_.load(std::memory_order_relaxed);
        const std::string handoff_error = handoff.take_error();
        std::error_code ec;
        const auto remove_partial = [&]() { std::filesystem::remove(encode.destination, ec); };
        if (cancelled) {
            remove_partial();
            postFinish(false, "cancelled");
            return;
        }
        if (!handoff_error.empty()) {
            remove_partial();
            postFinish(false, handoff_error);
            return;
        }
        if (!enc_out.ok) {
            remove_partial();
            postFinish(false, enc_out.error.empty() ? "encoding failed" : enc_out.error);
            return;
        }
        if (encoded_frames == 0) {
            remove_partial();
            postFinish(false, "the source delivered no frames");
            return;
        }
        postFinish(true, { });
    });
}

void EnhanceController::reportProgress(double fraction, const QString &stage)
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

void EnhanceController::postFinish(bool ok, const std::string &error)
{
    // The worker is returning; hand the outcome to the controller's thread so
    // the scheduler transition and the apply stay on the one thread that owns
    // them.
    QMetaObject::invokeMethod(
            this, [this, ok, error]() { finishEnhance(ok, QString::fromStdString(error)); },
            Qt::QueuedConnection);
}

void EnhanceController::finishEnhance(bool ok, const QString &error)
{
    // Runs on the controller's thread. Drive the scheduler to its terminal
    // state, fold the outcome into the properties and emit the signals, then
    // join the worker - which has already posted this call and is returning -
    // so the next run can safely reuse workerThread_.
    bool final_ok = ok;
    QString final_error = error;

    if (ok) {
        QString refusal;
        if (applyEnhanceCommand(refusal)) {
            scheduler_.complete(
                    runId_,
                    jobs::JobResult{ jobs::EnhanceResult{ runOutput_.path, runEncodeRequest_.width,
                                                          runEncodeRequest_.height } });
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

bool EnhanceController::applyEnhanceCommand(QString &refusal)
{
    if (editor_ == nullptr) {
        refusal = QStringLiteral("no project");
        return false;
    }
    // One ReplaceClipMedia: the clip re-points at the enhanced copy, keeping
    // its length, looks and name, and its in-point (the copy stands in frame
    // for frame). The bin keeps the original. One undo step.
    ReplaceClipMedia apply;
    apply.clip_id = runClipId_;
    apply.item = runOutput_;
    apply.source_start = std::nullopt;
    const auto result = editor_->apply(genesis::project::Command{ std::move(apply) });
    if (!result) {
        refusal = QString::fromStdString(std::string(result.error().message()));
        return false;
    }
    emit historyChanged();
    emit projectEdited();
    return true;
}

void EnhanceController::setState(const QString &value)
{
    if (state_ == value) {
        return;
    }
    state_ = value;
    emit stateChanged();
}

void EnhanceController::setRunning(bool value)
{
    if (running_ == value) {
        return;
    }
    running_ = value;
    emit runningChanged();
}

void EnhanceController::setProgress(double value)
{
    if (progress_ == value) {
        return;
    }
    progress_ = value;
    emit progressChanged();
}

void EnhanceController::setStage(const QString &value)
{
    if (stage_ == value) {
        return;
    }
    stage_ = value;
    emit stageChanged();
}

void EnhanceController::setError(const QString &value)
{
    if (error_ == value) {
        return;
    }
    error_ = value;
    emit errorChanged();
}
