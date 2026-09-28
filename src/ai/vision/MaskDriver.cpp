// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "ai/vision/MaskDriver.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace genesis::ai::vision {

MaskGenerationResult generate_masks(const MaskGenerationRequest &request, FrameSource source,
                                    SegmentFn segment, bool runtime_enabled,
                                    const ProgressFn &progress, const AbortFn &abort,
                                    const BrushRefineFn &brush_refine)
{
    MaskGenerationResult result;

    // The capability gate first: without the runtime there is nothing to run,
    // and the caller learns "disabled" before any frame is pulled or any file
    // is written. The caller supplies this flag (from
    // SegmentationProvider::available()), so the driver stays pure and the
    // disabled path is testable in a runtime-enabled build too.
    if (!runtime_enabled) {
        result.error = MaskGenerationError::Disabled;
        result.message = "disabled: ONNX Runtime not compiled in "
                         "(rebuild with GENESIS_AI_VISION=ON)";
        return result;
    }
    if (!segment) {
        result.error = MaskGenerationError::SegmentFailed;
        result.message = "no segmentation supplied";
        return result;
    }

    if (progress) {
        progress(0.0, "segmenting");
    }

    bool cancelled = false;
    std::string segment_error;

    // Bucket the smart strokes by the source frame they name, so the sink can
    // hand each frame its own strokes in one lookup instead of scanning on
    // every delivered frame.
    std::map<std::uint32_t, std::vector<BrushStroke>> strokes_by_frame;
    for (const BrushStroke &stroke : request.strokes) {
        strokes_by_frame[stroke.frame].push_back(stroke);
    }

    // The sink the frame source drives: one mask per delivered frame. It runs
    // on whatever thread the source drives (the caller's), so all of MaskStore,
    // the segment seam and the progress/abort callbacks stay on that thread.
    const FrameSourceOutcome outcome = source([&](const FrameView &frame,
                                                  std::uint32_t frame_index) {
        if (abort && abort()) {
            cancelled = true;
            return false;
        }

        const MaskKey key{ request.source,   request.source_size, request.subject,
                           request.model_id, frame_index,         request.revision };
        const std::filesystem::path path = mask_cache_path(request.mask_root, key);

        // Reuse: a mask already fresh for this key is skipped without a
        // segmentation run - the whole point of the on-disk store.
        if (mask_is_fresh(path, key)) {
            ++result.masks_reused;
            if (result.width == 0 && frame.width != 0) {
                result.width = frame.width;
                result.height = frame.height;
            }
            return true;
        }

        const SegmentationOutcome seg = segment(frame, progress, abort);
        if (!seg.ok) {
            if (seg.error == "cancelled" || (abort && abort())) {
                cancelled = true;
            } else {
                segment_error = seg.error.empty() ? "segmentation failed" : seg.error;
            }
            return !cancelled;
        }

        // The alpha plane is width*height floats in [0, 1]; the mask file
        // stores one 8-bit byte per pixel, stride = width (no padding).
        std::vector<float> alpha = seg.alpha;
        bool refined = false;
        if (brush_refine) {
            const auto it = strokes_by_frame.find(frame_index);
            if (it != strokes_by_frame.end()) {
                std::string refine_error;
                if (brush_refine(alpha, frame, it->second, refine_error)) {
                    refined = true;
                }
                // A refine failure keeps the base alpha: the cutout stays
                // correct and the stroke stays pending, so the run reports
                // success and the mask is written unrefined.
            }
        }

        MaskData mask;
        mask.width = seg.width;
        mask.height = seg.height;
        mask.alpha.resize(alpha.size());
        for (std::size_t i = 0; i < alpha.size(); ++i) {
            mask.alpha[i] = static_cast<std::uint8_t>(
                    std::lround(std::clamp(alpha[i], 0.0f, 1.0f) * 255.0f));
        }

        if (!write_mask(path, mask) || !mask_record(path, key) || !mask_is_fresh(path, key)) {
            segment_error = "cannot write mask for frame " + std::to_string(frame_index);
            return false;
        }
        ++result.masks_written;
        if (refined) {
            ++result.masks_refined;
        }
        result.width = mask.width;
        result.height = mask.height;
        return true;
    });

    // Fold the run's terminal state into the result. Cancellation wins over a
    // source or segmentation failure only when the caller actually asked to
    // stop; otherwise the first failure is the reason.
    if (cancelled || (abort && abort())) {
        result.error = MaskGenerationError::Cancelled;
        result.message = "cancelled";
    } else if (!outcome.ok) {
        result.error = MaskGenerationError::FrameSourceFailed;
        result.message = outcome.error.empty() ? "the frame source failed" : outcome.error;
    } else if (!segment_error.empty()) {
        result.error = MaskGenerationError::SegmentFailed;
        result.message = segment_error;
    } else if (result.masks_written + result.masks_reused == 0) {
        result.error = MaskGenerationError::NoFrames;
        result.message = "the source delivered no frames";
    }

    if (result.error == MaskGenerationError::None) {
        if (progress) {
            progress(1.0, "done");
        }
    }
    return result;
}

} // namespace genesis::ai::vision
