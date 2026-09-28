// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The vocabulary every vision provider and driver shares: one decoded frame and
// the progress/abort callbacks a long run reports through. They lived in
// SegmentationProvider.h, which made the other providers include a segmentation
// API for a frame view; they are independent of any one model.

#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

namespace genesis::ai::vision {

// One decoded frame: RGB 8-bit, row-major, three bytes per pixel (no alpha, no
// padding). The provider never acquires frames - video decode is the caller's
// business (docs/decisions/ai-provider.md: the interface takes frames). A null
// `rgb`, a zero width or a zero height is an empty frame and is refused.
struct FrameView
{
    const std::uint8_t *rgb = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// Progress: a fraction in 0..1 plus a stage the caller can show
// ("preprocessing", "segmenting", "done"). Called on the provider's own thread.
using ProgressFn = std::function<void(double fraction, std::string_view stage)>;

// Cancellation: return true to stop. Polled between stages - a single forward
// pass is not interruptible mid-run, but a true answer before the run turns the
// outcome into a graceful ok=false "cancelled".
using AbortFn = std::function<bool()>;

} // namespace genesis::ai::vision
