// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ai/vision/SegmentationProvider.h"

namespace genesis::ai::vision {

// The outcome of one enhancement. When ok is false, `error` carries a human
// reason and `rgb` is empty; nothing is half-filled on a failure.
struct EnhanceOutcome
{
    bool ok = false;
    std::string error;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // The enhanced picture: RGB 8-bit, row-major, width * height * 3 bytes.
    std::vector<std::uint8_t> rgb;
};

// The largest factor in {1, 2, 4} that keeps the longest output side at or
// under 4096 pixels, at most the requested one. The model always runs at 4x
// and the result is downscaled to source * factor, so a 2x ask on a large
// frame still gives 2x; only when even the requested factor would push the
// longest side past 4096 does the run fall back (a denoise-only 1x pass at the
// extreme). A request outside {1, 2, 4} snaps to the nearest supported step.
std::uint32_t enhance_factor(std::uint32_t width, std::uint32_t height, std::uint32_t requested);

// The ONNX enhancement provider (Real-ESRGAN), engine-free and Qt-free. It
// turns one RGB frame into a larger, cleaner one through a CPU ORT session,
// tiled so an arbitrarily large frame runs in bounded memory rather than as one
// huge tensor. The model is loaded once (load) and its session reused across
// enhance() calls - far too large to reload per frame. The session's lifetime
// is owned by the provider and freed on destruction on every path.
//
// When ONNX Runtime was not compiled in (the default build), available()
// returns false, status() reports "disabled", and load() yields a provider
// whose ok() is false with a "disabled" reason - the caller learns this at
// runtime rather than failing to link.
class EnhanceProvider
{
public:
    // Whether ONNX Runtime was compiled in. False in the default build
    // (GENESIS_AI_VISION=OFF).
    static bool available();

    // "enabled" or "disabled": the runtime state, for logs and UI.
    static std::string_view status();

    // Loads the ONNX model at `model_path` and returns a provider that owns
    // its CPU session. Returns a provider with ok()==false and a non-empty
    // error() when the runtime is disabled or the model fails to load. Never
    // throws.
    static EnhanceProvider load(std::string_view model_path);

    // Whether the model loaded. False for a default-constructed provider too.
    bool ok() const { return impl_ != nullptr; }
    // Why load() failed; empty when ok().
    const std::string &error() const { return error_; }

    // Enhances one frame to source * factor. Returns ok=false with a clear
    // reason when the runtime is disabled, the model failed to load, the frame
    // is empty, `factor` is not 1/2/4, or the run aborted.
    EnhanceOutcome enhance(const FrameView &frame, std::uint32_t factor,
                           const ProgressFn &progress = { }, const AbortFn &abort = { }) const;

    EnhanceProvider();
    EnhanceProvider(EnhanceProvider &&) noexcept;
    EnhanceProvider &operator=(EnhanceProvider &&) noexcept;
    ~EnhanceProvider();

    EnhanceProvider(const EnhanceProvider &) = delete;
    EnhanceProvider &operator=(const EnhanceProvider &) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string error_;
};

} // namespace genesis::ai::vision
