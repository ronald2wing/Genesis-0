// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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

// Which cutout model to run: person matting (RVM) or object segmentation
// (IS-Net). Mirrors the jobs::Subject resolution, named here by model so a
// caller never has to know a runtime's wiring.
enum class SegmentModel {
    Rvm, // person matting, rvm_mobilenetv3
    IsNet, // object, isnet-general-use
};

constexpr std::string_view segment_model_name(SegmentModel model)
{
    switch (model) {
    case SegmentModel::Rvm:
        return "rvm";
    case SegmentModel::IsNet:
        return "isnet";
    }
    return "rvm";
}

// The outcome of one segmentation. When ok is false, `error` carries a human
// reason and `alpha` is empty; nothing is half-filled on a failure.
struct SegmentationOutcome
{
    bool ok = false;
    std::string error;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // One value per pixel, in [0, 1], row-major: 0 = background (drop),
    // 1 = subject (keep). width * height values.
    std::vector<float> alpha;
};

// Progress: a fraction in 0..1 plus a stage ("preprocessing", "segmenting",
// "done"). Called on the calling thread.
using ProgressFn = std::function<void(double fraction, std::string_view stage)>;
// Cancellation: return true to stop. Polled between stages - a single forward
// pass is not interruptible mid-run, but a true answer before the run turns the
// outcome into a graceful ok=false "cancelled".
using AbortFn = std::function<bool()>;

// The ONNX segmentation provider, engine-free and Qt-free. It turns one RGB
// frame into a per-pixel alpha mask through a CPU ORT session. The model is
// loaded once (load) and the session is reused across segment() calls - a
// segmentation model is far too large to reload per frame. The session's
// lifetime is owned by the provider and freed on destruction on every path.
//
// When ONNX Runtime was not compiled in (the default build), available()
// returns false, status() reports "disabled", and load() yields a provider
// whose ok() is false with a "disabled" reason - the caller learns this at
// runtime rather than failing to link.
class SegmentationProvider
{
public:
    // Whether ONNX Runtime was compiled in. False in the default build
    // (GENESIS_AI_VISION=OFF).
    static bool available();

    // "enabled" or "disabled": the runtime state, for logs and UI.
    static std::string_view status();

    // Loads the ONNX model at `model_path` for `model` and returns a provider
    // that owns its CPU session. Returns a provider with ok()==false and a
    // non-empty error() when the runtime is disabled or the model fails to
    // load. Never throws.
    static SegmentationProvider load(std::string_view model_path, SegmentModel model);

    // Whether the model loaded. False for a default-constructed provider too.
    bool ok() const { return impl_ != nullptr; }
    // Why load() failed; empty when ok().
    const std::string &error() const { return error_; }

    // Segments one frame into an alpha mask. Returns ok=false with a clear
    // reason when the runtime is disabled, the model failed to load, the frame
    // is empty, or the run aborted.
    SegmentationOutcome segment(const FrameView &frame, const ProgressFn &progress = { },
                                const AbortFn &abort = { }) const;

    SegmentationProvider();
    SegmentationProvider(SegmentationProvider &&) noexcept;
    SegmentationProvider &operator=(SegmentationProvider &&) noexcept;
    ~SegmentationProvider();

    SegmentationProvider(const SegmentationProvider &) = delete;
    SegmentationProvider &operator=(const SegmentationProvider &) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string error_;
};

} // namespace genesis::ai::vision
