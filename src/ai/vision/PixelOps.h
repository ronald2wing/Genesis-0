// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The pixel arithmetic the vision providers share: clamped bilinear sampling,
// plane resizing and tensor wrapping. Every function here was copied verbatim
// into two or three providers; the bodies were identical, so they live once.
// Only the providers built with GENESIS_AI_VISION include this header.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <onnxruntime_cxx_api.h>

#include "ai/vision/VisionTypes.h"

namespace genesis::ai::vision {

inline float clampf(float value, float low, float high)
{
    return value < low ? low : (value > high ? high : value);
}

inline int clamp_int(int value, int low, int high)
{
    return value < low ? low : (value > high ? high : value);
}

// Bilinear sample of the interleaved RGB frame at continuous (x, y) in frame
// coordinates, clamped at the edges. Writes three values in 0..255.
inline void sample_rgb(const FrameView &frame, float x, float y, float out[3])
{
    const int w = static_cast<int>(frame.width);
    const int h = static_cast<int>(frame.height);
    const int x0 = clamp_int(static_cast<int>(std::floor(x)), 0, w - 1);
    const int y0 = clamp_int(static_cast<int>(std::floor(y)), 0, h - 1);
    const int x1 = clamp_int(x0 + 1, 0, w - 1);
    const int y1 = clamp_int(y0 + 1, 0, h - 1);
    const float tx = clampf(x - static_cast<float>(x0), 0.0f, 1.0f);
    const float ty = clampf(y - static_cast<float>(y0), 0.0f, 1.0f);
    for (int channel = 0; channel < 3; ++channel) {
        const float a = frame.rgb[(y0 * w + x0) * 3 + channel];
        const float b = frame.rgb[(y0 * w + x1) * 3 + channel];
        const float d = frame.rgb[(y1 * w + x0) * 3 + channel];
        const float e = frame.rgb[(y1 * w + x1) * 3 + channel];
        out[channel] = (a * (1.0f - tx) + b * tx) * (1.0f - ty) + (d * (1.0f - tx) + e * tx) * ty;
    }
}

// Bilinear sample of one plane (row-major, sw x sh) at continuous (x, y),
// clamped at the edges.
inline float sample_plane(const float *plane, std::uint32_t sw, std::uint32_t sh, float x, float y)
{
    const int w = static_cast<int>(sw);
    const int h = static_cast<int>(sh);
    const int x0 = clamp_int(static_cast<int>(std::floor(x)), 0, w - 1);
    const int y0 = clamp_int(static_cast<int>(std::floor(y)), 0, h - 1);
    const int x1 = clamp_int(x0 + 1, 0, w - 1);
    const int y1 = clamp_int(y0 + 1, 0, h - 1);
    const float tx = clampf(x - static_cast<float>(x0), 0.0f, 1.0f);
    const float ty = clampf(y - static_cast<float>(y0), 0.0f, 1.0f);
    const float a = plane[y0 * w + x0];
    const float b = plane[y0 * w + x1];
    const float d = plane[y1 * w + x0];
    const float e = plane[y1 * w + x1];
    return (a * (1.0f - tx) + b * tx) * (1.0f - ty) + (d * (1.0f - tx) + e * tx) * ty;
}

// Resizes a low-res alpha plane (row-major, sw x sh) to (dw, dh), clamped to
// 0..1.
inline std::vector<float> resize_plane(const std::vector<float> &source, std::uint32_t sw,
                                       std::uint32_t sh, std::uint32_t dw, std::uint32_t dh)
{
    std::vector<float> out(static_cast<std::size_t>(dw) * dh);
    for (std::uint32_t y = 0; y < dh; ++y) {
        const float fy = (static_cast<float>(y) + 0.5f) * sh / dh - 0.5f;
        for (std::uint32_t x = 0; x < dw; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f) * sw / dw - 0.5f;
            out[static_cast<std::size_t>(y) * dw + x] =
                    clampf(sample_plane(source.data(), sw, sh, fx, fy), 0.0f, 1.0f);
        }
    }
    return out;
}

// Wraps `data` (count = product of `shape`) as a CPU float tensor owned by the
// caller for the lifetime of the returned value.
inline Ort::Value tensor(const Ort::MemoryInfo &info, const std::vector<std::int64_t> &shape,
                         float *data)
{
    std::size_t count = 1;
    for (const std::int64_t dimension : shape) {
        count *= static_cast<std::size_t>(dimension);
    }
    return Ort::Value::CreateTensor<float>(info, data, count, shape.data(), shape.size());
}

} // namespace genesis::ai::vision
