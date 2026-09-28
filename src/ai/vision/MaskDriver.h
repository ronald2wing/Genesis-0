// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "ai/vision/MaskStore.h"
#include "ai/vision/SegmentationProvider.h"
#include "jobs/Job.h"

namespace genesis::ai::vision {

// ---------------------------------------------------------------------------
// The mask-generation driver
// ---------------------------------------------------------------------------
//
// Turns a stream of decoded frames into per-frame alpha masks on disk, reusing
// any mask that is already fresh. It is the host-side, engine-free half of
// Stage 2b-i: the frame source (which may reach a GStreamer decode) and the
// segmentation (which may reach the ONNX runtime) are both injected, so the
// driver's own logic - keying, reuse, stride metadata, cancel - is testable
// with synthetic frames and a fake provider and needs no runtime to load.
//
// A mask is cut at the frame's own size and stored through MaskStore: the mask
// file's header records the width and height, the alpha plane is width bytes
// per row (stride == width, no padding), and the ".key" sidecar records the
// source, subject, model and frame index. Together they are the sidecar
// metadata a later reader (or a rerun) needs to decide reuse.

// One smart stroke to refine a base mask at: the source frame it was painted
// at, and the stroke's tool + points. The plain discs (Brush/Eraser) are not
// smart - they paint a disc, not a model refinement - so only the smart tools
// ride here.
struct BrushStroke
{
    // The source frame the stroke names (the frame a smart brush reads the
    // thing under it from).
    std::uint32_t frame = 0;
    // Which smart tool: SmartBrush (keep) or SmartEraser (drop).
    genesis::project::BrushTool tool = genesis::project::BrushTool::SmartBrush;
    // The stroke's points, as source fractions ([0,1] across the frame).
    std::vector<std::array<double, 2>> points;
};

// What to mask and where the masks land.
struct MaskGenerationRequest
{
    // The media file the frames were decoded from; rides into every mask key.
    std::filesystem::path source;
    // Its byte size at run time - the cheap "the file changed" proxy the
    // staleness comparison uses. 0 never matches a real file, so a run that
    // forgets to set it simply regenerates every mask.
    std::uintmax_t source_size = 0;
    // Which subject the model looked for.
    jobs::Subject subject = jobs::Subject::Auto;
    // Which model ran, e.g. "rvm" or "isnet". Recorded on the key, never used
    // to drive anything here.
    std::string model_id;
    // The directory the mask files live under.
    std::filesystem::path mask_root;
    // The stroke revision the masks are keyed by (mask_revision()). Empty for
    // an automatic cutout, so those runs keep keying the base mask.
    std::string revision;
    // The smart strokes to refine the base masks with, one entry per painted
    // stroke. The driver groups them by their source frame and calls the
    // refine seam for the frames that carry one; empty for an automatic cutout
    // or a custom one painted only with plain discs.
    std::vector<BrushStroke> strokes;
};

// Why a mask run failed. Distinct codes so the caller can react without
// parsing text. No exception crosses the seam.
enum class MaskGenerationError {
    None, // success
    Disabled, // the segmentation runtime is not compiled in
    FrameSourceFailed, // the injected frame source reported a failure
    SegmentFailed, // the injected segmentation failed for a frame
    WriteFailed, // a mask could not be written or recorded
    NoFrames, // the source delivered no frames (a clean but empty run)
    Cancelled, // the caller asked to stop
};

// What a mask run reports back. On success `error` is None and the counters
// sum the frames processed; on failure `error` is set and `message` carries a
// human-readable reason. Masks written before a mid-run failure stay on disk -
// they are valid, and a rerun reuses them.
struct MaskGenerationResult
{
    MaskGenerationError error = MaskGenerationError::None;
    std::string message;
    // Masks newly written this run (segmented then stored).
    std::uint64_t masks_written = 0;
    // Masks found already fresh and skipped (no segmentation ran for them).
    std::uint64_t masks_reused = 0;
    // Masks whose base alpha a smart stroke refined this run.
    std::uint64_t masks_refined = 0;
    // The frame size the masks were cut at (zero when no frame was processed).
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// Called once per delivered frame. Return false to stop the source early (the
// driver returns false here when the run is cancelled, so a cancel propagates
// through the source without the source knowing why).
using FrameSink = std::function<bool(const FrameView &frame, std::uint32_t frame_index)>;

// A frame source: pumps frames into `sink` until it is done. `ok` is false
// when the source itself failed (with `error` carrying the reason); a true
// result after the sink asked to stop is a normal, not a failed, completion.
// The real wiring wraps ges::extract_video_frames; tests pump synthetic
// frames.
struct FrameSourceOutcome
{
    bool ok = false;
    std::string error;
};
using FrameSource = std::function<FrameSourceOutcome(const FrameSink &)>;

// Segments one frame into an alpha mask. This is the seam the real
// SegmentationProvider plugs into (the caller wires a lambda reaching
// provider.segment); tests inject a fake returning canned alpha, so no model
// and no runtime are needed to exercise the driver.
using SegmentFn =
        std::function<SegmentationOutcome(const FrameView &, const ProgressFn &, const AbortFn &)>;

// Refines a base alpha at one frame for the strokes painted there. Given the
// frame's alpha (width*height floats in [0,1], row-major), the frame itself
// and the strokes that name it, it refines the alpha in place. Return false
// with a reason when the refinement cannot run - the driver then keeps the
// base alpha, so a refine failure is non-fatal: the cutout stays correct and
// the stroke stays pending. The real wiring reaches BrushProvider; tests
// inject a fake.
using BrushRefineFn =
        std::function<bool(std::vector<float> &alpha, const FrameView &frame,
                           const std::vector<BrushStroke> &strokes, std::string &error)>;

// Runs the mask generation: pumps `source`, segments each frame through
// `segment`, stores the alpha as a mask and records its key, reusing any mask
// that is already fresh. `runtime_enabled` is the caller's own capability
// check (SegmentationProvider::available()); when false the driver reports
// MaskGenerationError::Disabled without pulling a frame, so a runtime-free
// build still gets a clean "disabled" rather than a crash. `brush_refine`,
// when set and the request carries strokes, refines the base alpha of the
// frames a stroke names; a refine failure keeps the base alpha (non-fatal).
// Blocking; run it off the UI thread.
MaskGenerationResult generate_masks(const MaskGenerationRequest &request, FrameSource source,
                                    SegmentFn segment, bool runtime_enabled,
                                    const ProgressFn &progress = { }, const AbortFn &abort = { },
                                    const BrushRefineFn &brush_refine = { });

} // namespace genesis::ai::vision
