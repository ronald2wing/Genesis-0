// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "ai/vision/MaskDriver.h"

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const std::string &what)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

namespace vision = genesis::ai::vision;
namespace jobs = genesis::jobs;

constexpr std::uint32_t kW = 4;
constexpr std::uint32_t kH = 3;

// A per-run sandbox under the system temp dir, emptied first so a crashed run
// cannot leak a fresh-looking mask into the assertions.
const std::filesystem::path &sandbox()
{
    static const std::filesystem::path dir = [] {
        const std::filesystem::path p =
                std::filesystem::temp_directory_path() / "genesis-mask-driver-tests";
        std::filesystem::remove_all(p);
        std::filesystem::create_directories(p);
        return p;
    }();
    return dir;
}

// A synthetic RGB frame (a small gradient), so the driver sees a real,
// non-empty buffer without any decode.
std::vector<std::uint8_t> frame_bytes()
{
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(kW) * kH * 3);
    for (std::size_t i = 0; i < rgb.size(); ++i) {
        rgb[i] = static_cast<std::uint8_t>(i * 7 + 1);
    }
    return rgb;
}

vision::MaskGenerationRequest request()
{
    // A fresh root per test call, so a mask written by one test cannot leak a
    // fresh-looking key into another (each test drives the same source/subject/
    // model, so without this they would reuse each other's frame 0).
    static int root_counter = 0;
    vision::MaskGenerationRequest r;
    r.source = "/media/clips/interview.mp4";
    r.source_size = 123456789;
    r.subject = jobs::Subject::Person;
    r.model_id = "rvm";
    r.mask_root = sandbox() / ("masks-" + std::to_string(root_counter++));
    return r;
}

// A fake segment seam: fills the alpha with a constant 0.0 (any constant the
// reader can compare), and counts calls so a test can prove reuse skips it.
struct FakeSegment
{
    int calls = 0;
    vision::SegmentationOutcome operator()(const vision::FrameView &frame,
                                           const vision::ProgressFn &, const vision::AbortFn &)
    {
        ++calls;
        vision::SegmentationOutcome o;
        o.ok = true;
        o.width = frame.width;
        o.height = frame.height;
        o.alpha.assign(static_cast<std::size_t>(frame.width) * frame.height, 0.0f);
        return o;
    }
};

// A source that pumps `indexes` once through the sink.
vision::FrameSource source_for(std::vector<std::uint32_t> indexes,
                               const std::vector<std::uint8_t> &rgb)
{
    return [indexes = std::move(indexes), &rgb](const vision::FrameSink &sink) {
        for (const std::uint32_t index : indexes) {
            if (!sink(vision::FrameView{ rgb.data(), kW, kH }, index)) {
                break;
            }
        }
        return vision::FrameSourceOutcome{ true, { } };
    };
}

// Writing then reusing: the first run segments and stores, the second reuses
// every mask and never calls the segment seam.
void test_write_then_reuse()
{
    const vision::MaskGenerationRequest req = request();
    const std::vector<std::uint8_t> rgb = frame_bytes();

    FakeSegment first_segment;
    const vision::MaskGenerationResult first = vision::generate_masks(
            req, source_for({ 0, 3 }, rgb),
            [&](const vision::FrameView &f, const vision::ProgressFn &p, const vision::AbortFn &a) {
                return first_segment(f, p, a);
            },
            true);

    check(first.error == vision::MaskGenerationError::None, "the first run succeeds");
    check(first.masks_written == 2,
          "two masks are written (got " + std::to_string(first.masks_written) + ")");
    check(first.masks_reused == 0, "nothing is reused on the first run");
    check(first.width == kW && first.height == kH, "the result reports the frame dimensions");
    check(first_segment.calls == 2,
          "the segment seam ran twice (got " + std::to_string(first_segment.calls) + ")");

    // The two frame indexes key two distinct, now-fresh masks.
    const vision::MaskKey k0{ req.source, req.source_size, req.subject, req.model_id,
                              0,          req.revision };
    const vision::MaskKey k3{ req.source, req.source_size, req.subject, req.model_id,
                              3,          req.revision };
    const std::filesystem::path p0 = vision::mask_cache_path(req.mask_root, k0);
    const std::filesystem::path p3 = vision::mask_cache_path(req.mask_root, k3);
    check(vision::mask_is_fresh(p0, k0), "frame 0's mask is fresh on disk");
    check(vision::mask_is_fresh(p3, k3), "frame 3's mask is fresh on disk");
    check(p0 != p3, "two frames map to two files");

    FakeSegment second_segment;
    const vision::MaskGenerationResult second = vision::generate_masks(
            req, source_for({ 0, 3 }, rgb),
            [&](const vision::FrameView &f, const vision::ProgressFn &p, const vision::AbortFn &a) {
                return second_segment(f, p, a);
            },
            true);

    check(second.error == vision::MaskGenerationError::None, "the second run succeeds");
    check(second.masks_written == 0, "the second run writes nothing");
    check(second.masks_reused == 2, "the second run reuses both masks");
    check(second_segment.calls == 0, "the second run never segments (reuse is pure)");
}

// Stride and dimensions survive to disk: a written mask reads back with its
// width, height, and a width*height alpha plane (stride == width).
void test_stride_and_dims()
{
    const vision::MaskGenerationRequest req = request();
    const std::vector<std::uint8_t> rgb = frame_bytes();
    const vision::MaskGenerationResult result = vision::generate_masks(
            req, source_for({ 1 }, rgb),
            [](const vision::FrameView &f, const vision::ProgressFn &, const vision::AbortFn &) {
                vision::SegmentationOutcome o;
                o.ok = true;
                o.width = f.width;
                o.height = f.height;
                o.alpha.assign(static_cast<std::size_t>(f.width) * f.height, 0.75f);
                return o;
            },
            true);

    check(result.error == vision::MaskGenerationError::None, "the run succeeds");
    const vision::MaskKey key{ req.source, req.source_size, req.subject, req.model_id,
                               1,          req.revision };
    const std::optional<vision::MaskData> mask =
            vision::read_mask(vision::mask_cache_path(req.mask_root, key));
    check(mask.has_value(), "the mask reads back");
    if (!mask) {
        return;
    }
    check(mask->width == kW && mask->height == kH, "the mask records the frame dimensions");
    check(mask->alpha.size() == static_cast<std::size_t>(kW) * kH,
          "the alpha plane is width * height (stride == width, no padding)");
    check(mask->alpha.front() == 191 && mask->alpha.back() == 191,
          "0.75 maps to 191 bytes at the edges");
}

// A mid-run cancel stops the source and reports Cancelled, leaving only the
// masks already produced.
void test_cancel()
{
    const vision::MaskGenerationRequest req = request();
    const std::vector<std::uint8_t> rgb = frame_bytes();
    int segment_calls = 0;
    int frames_seen = 0;

    // A source that pumps many frames. The driver polls abort at the top of
    // each frame; the segment seam advances `frames_seen`, so after the third
    // frame the next poll returns true, the sink refuses, and the source stops.
    const vision::MaskGenerationResult result = vision::generate_masks(
            req,
            [&](const vision::FrameSink &sink) {
                for (std::uint32_t i = 0; i < 10; ++i) {
                    if (!sink(vision::FrameView{ rgb.data(), kW, kH }, i)) {
                        return vision::FrameSourceOutcome{ true, { } };
                    }
                }
                return vision::FrameSourceOutcome{ true, { } };
            },
            [&](const vision::FrameView &f, const vision::ProgressFn &, const vision::AbortFn &) {
                ++segment_calls;
                ++frames_seen;
                vision::SegmentationOutcome o;
                o.ok = true;
                o.width = f.width;
                o.height = f.height;
                o.alpha.assign(static_cast<std::size_t>(f.width) * f.height, 0.5f);
                return o;
            },
            true, nullptr, [&]() { return frames_seen >= 3; });

    check(result.error == vision::MaskGenerationError::Cancelled,
          "a mid-run cancel reports Cancelled");
    check(segment_calls == 3,
          "the cancel stops after three segments (got " + std::to_string(segment_calls) + ")");
    check(result.masks_written == 3, "the masks already produced survive");
}

// A frame source that fails reports FrameSourceFailed with the source's own
// reason.
void test_frame_source_failure()
{
    const vision::MaskGenerationRequest req = request();
    const vision::MaskGenerationResult result = vision::generate_masks(
            req,
            [](const vision::FrameSink &) {
                return vision::FrameSourceOutcome{ false, "decode exploded" };
            },
            [](const vision::FrameView &f, const vision::ProgressFn &, const vision::AbortFn &) {
                vision::SegmentationOutcome o;
                o.ok = true;
                o.width = f.width;
                o.height = f.height;
                o.alpha.assign(static_cast<std::size_t>(f.width) * f.height, 0.5f);
                return o;
            },
            true);

    check(result.error == vision::MaskGenerationError::FrameSourceFailed,
          "a failing source reports FrameSourceFailed");
    check(result.message == "decode exploded", "the source's own reason rides through");
}

// A clean run with no frames reports NoFrames, not success.
void test_no_frames()
{
    const vision::MaskGenerationRequest req = request();
    const vision::MaskGenerationResult result = vision::generate_masks(
            req, [](const vision::FrameSink &) { return vision::FrameSourceOutcome{ true, { } }; },
            [](const vision::FrameView &f, const vision::ProgressFn &, const vision::AbortFn &) {
                vision::SegmentationOutcome o;
                o.ok = true;
                o.width = f.width;
                o.height = f.height;
                o.alpha.assign(static_cast<std::size_t>(f.width) * f.height, 0.5f);
                return o;
            },
            true);

    check(result.error == vision::MaskGenerationError::NoFrames, "an empty run reports NoFrames");
}

// A segment seam that fails reports SegmentFailed with the seam's reason, and
// no mask lands.
void test_segment_failure()
{
    const vision::MaskGenerationRequest req = request();
    const std::vector<std::uint8_t> rgb = frame_bytes();
    const vision::MaskGenerationResult result = vision::generate_masks(
            req, source_for({ 0 }, rgb),
            [](const vision::FrameView &, const vision::ProgressFn &, const vision::AbortFn &) {
                vision::SegmentationOutcome o;
                o.ok = false;
                o.error = "inference failed";
                return o;
            },
            true);

    check(result.error == vision::MaskGenerationError::SegmentFailed,
          "a failing segment reports SegmentFailed");
    check(result.message == "inference failed", "the seam's reason rides through");
    check(result.masks_written == 0, "no mask lands on a segment failure");
}

// A successful run reports progress (a start and a done).
void test_progress_reported()
{
    const vision::MaskGenerationRequest req = request();
    const std::vector<std::uint8_t> rgb = frame_bytes();
    int reports = 0;
    double first_fraction = -1.0;
    double last_fraction = -1.0;
    const vision::MaskGenerationResult result = vision::generate_masks(
            req, source_for({ 0 }, rgb),
            [](const vision::FrameView &f, const vision::ProgressFn &, const vision::AbortFn &) {
                vision::SegmentationOutcome o;
                o.ok = true;
                o.width = f.width;
                o.height = f.height;
                o.alpha.assign(static_cast<std::size_t>(f.width) * f.height, 0.5f);
                return o;
            },
            true,
            [&](double fraction, std::string_view) {
                if (reports == 0) {
                    first_fraction = fraction;
                }
                last_fraction = fraction;
                ++reports;
            });

    check(result.error == vision::MaskGenerationError::None, "the run succeeds");
    check(reports >= 2, "progress is reported (got " + std::to_string(reports) + ")");
    check(first_fraction == 0.0, "the first report is the start (0.0)");
    check(last_fraction == 1.0, "the last report is done (1.0)");
}

// The disabled runtime is reported cleanly, before any frame is pulled or any
// file written. The flag is injected, so this runs in both the default and the
// runtime-enabled build.
void test_disabled_reports_cleanly()
{
    const vision::MaskGenerationRequest req = request();
    const std::vector<std::uint8_t> rgb = frame_bytes();
    bool source_ran = false;
    const vision::MaskGenerationResult result = vision::generate_masks(
            req,
            [&](const vision::FrameSink &) {
                source_ran = true;
                return vision::FrameSourceOutcome{ true, { } };
            },
            [](const vision::FrameView &f, const vision::ProgressFn &, const vision::AbortFn &) {
                vision::SegmentationOutcome o;
                o.ok = true;
                o.width = f.width;
                o.height = f.height;
                o.alpha.assign(static_cast<std::size_t>(f.width) * f.height, 0.5f);
                return o;
            },
            false);

    check(result.error == vision::MaskGenerationError::Disabled,
          "a disabled runtime reports Disabled");
    check(result.message.find("disabled") != std::string::npos,
          "the reason reports the disabled runtime");
    check(!source_ran, "no frame is pulled when disabled");
}

// A brush refine seam that succeeds: the driver groups the request's strokes by
// their frame, hands each frame's strokes to the seam once, counts the refined
// mask, and writes the seam's refined alpha (not the base) to disk.
void test_refine_applies_and_counts()
{
    vision::MaskGenerationRequest req = request();
    vision::BrushStroke stroke;
    stroke.frame = 0;
    stroke.tool = genesis::project::BrushTool::SmartBrush;
    stroke.points = { { 0.5, 0.5 } };
    req.strokes.push_back(stroke);
    const std::vector<std::uint8_t> rgb = frame_bytes();

    int refine_calls = 0;
    const vision::MaskGenerationResult result = vision::generate_masks(
            req, source_for({ 0, 1 }, rgb),
            [](const vision::FrameView &f, const vision::ProgressFn &, const vision::AbortFn &) {
                vision::SegmentationOutcome o;
                o.ok = true;
                o.width = f.width;
                o.height = f.height;
                o.alpha.assign(static_cast<std::size_t>(f.width) * f.height, 0.0f);
                return o;
            },
            true, nullptr, nullptr,
            [&](std::vector<float> &alpha, const vision::FrameView &frame,
                const std::vector<vision::BrushStroke> &strokes, std::string &) {
                ++refine_calls;
                check(strokes.size() == 1, "the seam gets the frame's one stroke");
                check(strokes[0].frame == 0, "the stroke names frame 0");
                check(alpha.size() == static_cast<std::size_t>(frame.width) * frame.height,
                      "the seam gets the frame's alpha plane");
                for (float &v : alpha) {
                    v = 1.0f;
                }
                return true;
            });

    check(result.error == vision::MaskGenerationError::None, "the run succeeds");
    check(refine_calls == 1, "the refine seam ran once (got " + std::to_string(refine_calls) + ")");
    check(result.masks_refined == 1, "one mask is counted as refined");

    const vision::MaskKey key{ req.source, req.source_size, req.subject, req.model_id,
                               0,          req.revision };
    const std::optional<vision::MaskData> mask =
            vision::read_mask(vision::mask_cache_path(req.mask_root, key));
    check(mask.has_value(), "the refined mask reads back");
    if (mask) {
        check(mask->alpha.front() == 255 && mask->alpha.back() == 255,
              "the refined alpha (1.0) lands on disk, not the base 0.0");
    }
}

// A brush refine seam that fails is non-fatal: the driver keeps the base alpha,
// still writes the mask, and reports a successful run (the stroke stays
// pending rather than corrupting the cutout).
void test_refine_failure_non_fatal()
{
    vision::MaskGenerationRequest req = request();
    vision::BrushStroke stroke;
    stroke.frame = 0;
    stroke.tool = genesis::project::BrushTool::SmartBrush;
    stroke.points = { { 0.5, 0.5 } };
    req.strokes.push_back(stroke);
    const std::vector<std::uint8_t> rgb = frame_bytes();

    const vision::MaskGenerationResult result = vision::generate_masks(
            req, source_for({ 0 }, rgb),
            [](const vision::FrameView &f, const vision::ProgressFn &, const vision::AbortFn &) {
                vision::SegmentationOutcome o;
                o.ok = true;
                o.width = f.width;
                o.height = f.height;
                o.alpha.assign(static_cast<std::size_t>(f.width) * f.height, 0.0f);
                return o;
            },
            true, nullptr, nullptr,
            [](std::vector<float> &, const vision::FrameView &,
               const std::vector<vision::BrushStroke> &, std::string &error) {
                error = "model exploded";
                return false;
            });

    check(result.error == vision::MaskGenerationError::None,
          "a refine failure does not fail the run");
    check(result.masks_refined == 0, "nothing is counted as refined");
    check(result.masks_written == 1, "the base mask is still written");

    const vision::MaskKey key{ req.source, req.source_size, req.subject, req.model_id,
                               0,          req.revision };
    const std::optional<vision::MaskData> mask =
            vision::read_mask(vision::mask_cache_path(req.mask_root, key));
    check(mask.has_value(), "the base mask reads back");
    if (mask) {
        check(mask->alpha.front() == 0 && mask->alpha.back() == 0,
              "the base alpha (0.0) is kept on a refine failure");
    }
}

} // namespace

int main()
{
    test_write_then_reuse();
    test_stride_and_dims();
    test_cancel();
    test_frame_source_failure();
    test_no_frames();
    test_segment_failure();
    test_progress_reported();
    test_disabled_reports_cleanly();
    test_refine_applies_and_counts();
    test_refine_failure_non_fatal();

    std::filesystem::remove_all(sandbox());

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
