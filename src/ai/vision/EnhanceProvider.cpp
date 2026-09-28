// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "ai/vision/EnhanceProvider.h"

#include <algorithm>
#include <cmath>

namespace genesis::ai::vision {

// Shared by both builds: a pure arithmetic helper the controller calls before
// a run to settle the effective factor, no runtime needed.
std::uint32_t enhance_factor(std::uint32_t width, std::uint32_t height, std::uint32_t requested)
{
    const std::uint32_t ask = requested >= 4 ? 4 : (requested >= 2 ? 2 : 1);
    const std::uint64_t longest = static_cast<std::uint64_t>(width > height ? width : height);
    for (const std::uint32_t candidate : { 4U, 2U, 1U }) {
        if (candidate <= ask && longest * candidate <= 4096) {
            return candidate;
        }
    }
    return 1;
}

} // namespace genesis::ai::vision

#if defined(GENESIS_AI_VISION_ENABLED)

#  include "ai/vision/ExecutionProvider.h"

#  include <cstddef>
#  include <cstdint>
#  include <memory>
#  include <string>
#  include <utility>
#  include <vector>

#  include <onnxruntime_cxx_api.h>

namespace genesis::ai::vision {

namespace {

// The Real-ESRGAN general x4v3 model runs at exactly 4x. Tiles are the core
// region (kCore) plus a pad ring the model sees for context, so neighbouring
// tiles agree at their seams and the output is cropped back to the core before
// the downscale to the requested factor.
constexpr std::uint32_t kScale = 4;
constexpr std::uint32_t kCore = 256;
constexpr std::uint32_t kPad = 16;

float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

int clamp_int(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Bilinear sample of one plane (row-major, sw x sh) at continuous (x, y),
// clamped at the edges.
float sample_plane(const float *plane, std::uint32_t sw, std::uint32_t sh, float x, float y)
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

// Wraps `data` (count = product of `shape`) as a CPU float tensor owned by the
// caller for the lifetime of the returned value.
Ort::Value tensor(const Ort::MemoryInfo &info, const std::vector<std::int64_t> &shape, float *data)
{
    std::size_t count = 1;
    for (const std::int64_t d : shape) {
        count *= static_cast<std::size_t>(d);
    }
    return Ort::Value::CreateTensor<float>(info, data, count, shape.data(), shape.size());
}

} // namespace

struct EnhanceProvider::Impl
{
    Ort::Env env{ nullptr };
    Ort::Session session{ nullptr };
    std::vector<std::string> input_names;
    std::vector<std::string> output_names;
};

bool EnhanceProvider::available()
{
    return true;
}

std::string_view EnhanceProvider::status()
{
    return "enabled";
}

EnhanceProvider EnhanceProvider::load(std::string_view model_path)
{
    EnhanceProvider provider;
    if (model_path.empty()) {
        provider.error_ = "no model path";
        return provider;
    }

    auto impl = std::make_unique<Impl>();
    try {
        impl->env = Ort::Env(ORT_LOGGING_LEVEL_WARNING, "genesis-ai-enhance");
        const std::string path(model_path);
        impl->session = genesis::ai::make_session(impl->env, path.c_str());
    } catch (const Ort::Exception &e) {
        provider.error_ = "failed to load model: " + std::string(e.what());
        return provider;
    }

    // Record the input/output names this exact graph expects, so enhance()
    // feeds them in the model's own order rather than assuming a fixed one.
    Ort::AllocatorWithDefaultOptions allocator;
    const std::size_t n_in = impl->session.GetInputCount();
    impl->input_names.reserve(n_in);
    for (std::size_t i = 0; i < n_in; ++i) {
        impl->input_names.push_back(impl->session.GetInputNameAllocated(i, allocator).get());
    }
    const std::size_t n_out = impl->session.GetOutputCount();
    impl->output_names.reserve(n_out);
    for (std::size_t i = 0; i < n_out; ++i) {
        impl->output_names.push_back(impl->session.GetOutputNameAllocated(i, allocator).get());
    }

    // The enhance graph is single-in, single-out (RGB in, 4x RGB out).
    if (impl->input_names.size() != 1 || impl->output_names.size() != 1) {
        provider.error_ = "unexpected model I/O: expected one input and one "
                          "output, got "
                + std::to_string(impl->input_names.size()) + " and "
                + std::to_string(impl->output_names.size());
        return provider;
    }

    provider.impl_ = std::move(impl);
    return provider;
}

EnhanceOutcome EnhanceProvider::enhance(const FrameView &frame, std::uint32_t factor,
                                        const ProgressFn &progress, const AbortFn &abort) const
{
    EnhanceOutcome outcome;
    if (impl_ == nullptr) {
        outcome.error = error_.empty() ? "not loaded" : error_;
        return outcome;
    }
    if (frame.rgb == nullptr || frame.width == 0 || frame.height == 0) {
        outcome.error = "empty frame";
        return outcome;
    }
    if (factor != 1 && factor != 2 && factor != 4) {
        outcome.error = "unsupported factor";
        return outcome;
    }

    if (progress) {
        progress(0.0, "preprocessing");
    }
    if (abort && abort()) {
        outcome.error = "cancelled";
        return outcome;
    }

    const std::uint32_t src_w = frame.width;
    const std::uint32_t src_h = frame.height;
    const std::uint32_t out_w = src_w * factor;
    const std::uint32_t out_h = src_h * factor;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(out_w) * out_h * 3);

    const Ort::MemoryInfo info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    const std::uint32_t tiles_x = (src_w + kCore - 1) / kCore;
    const std::uint32_t tiles_y = (src_h + kCore - 1) / kCore;
    const std::uint32_t total_tiles = tiles_x * tiles_y;
    std::uint32_t tile_index = 0;

    try {
        for (std::uint32_t ty = 0; ty < tiles_y; ++ty) {
            const std::uint32_t core_y0 = ty * kCore;
            const std::uint32_t core_y1 = std::min(core_y0 + kCore, src_h);
            const std::uint32_t pad_y0 = core_y0 >= kPad ? core_y0 - kPad : 0;
            const std::uint32_t pad_y1 = std::min(core_y1 + kPad, src_h);
            const std::uint32_t window_h = pad_y1 - pad_y0;

            for (std::uint32_t tx = 0; tx < tiles_x; ++tx) {
                if (abort && abort()) {
                    outcome.error = "cancelled";
                    return outcome;
                }
                const std::uint32_t core_x0 = tx * kCore;
                const std::uint32_t core_x1 = std::min(core_x0 + kCore, src_w);
                const std::uint32_t pad_x0 = core_x0 >= kPad ? core_x0 - kPad : 0;
                const std::uint32_t pad_x1 = std::min(core_x1 + kPad, src_w);
                const std::uint32_t window_w = pad_x1 - pad_x0;

                // The padded window as an NCHW float tensor in [0, 1].
                std::vector<float> input(3ull * window_h * window_w);
                for (std::uint32_t y = 0; y < window_h; ++y) {
                    const std::size_t row = static_cast<std::size_t>(pad_y0 + y) * src_w;
                    const std::size_t base = row * 3;
                    for (std::uint32_t x = 0; x < window_w; ++x) {
                        const std::uint8_t *p = frame.rgb + base + (pad_x0 + x) * 3;
                        const std::size_t at = static_cast<std::size_t>(y) * window_w + x;
                        input[at] = p[0] / 255.0f;
                        input[window_h * window_w + at] = p[1] / 255.0f;
                        input[2 * window_h * window_w + at] = p[2] / 255.0f;
                    }
                }

                std::vector<Ort::Value> inputs;
                inputs.push_back(tensor(info,
                                        { 1, 3, static_cast<std::int64_t>(window_h),
                                          static_cast<std::int64_t>(window_w) },
                                        input.data()));
                std::vector<const char *> input_ptrs{ impl_->input_names[0].c_str() };
                std::vector<const char *> output_ptrs{ impl_->output_names[0].c_str() };
                std::vector<Ort::Value> outputs =
                        impl_->session.Run(Ort::RunOptions{ nullptr }, input_ptrs.data(),
                                           inputs.data(), inputs.size(), output_ptrs.data(), 1);

                const Ort::Value &result = outputs[0];
                const auto shape = result.GetTensorTypeAndShapeInfo().GetShape();
                if (shape.size() != 4) {
                    outcome.error = "unexpected output rank";
                    return outcome;
                }
                const float *data = result.GetTensorData<float>();
                const std::uint32_t model_h = static_cast<std::uint32_t>(shape[2]);
                const std::uint32_t model_w = static_cast<std::uint32_t>(shape[3]);

                // Crop the pad off the 4x output, then downscale the core to
                // source * factor and write it at the tile's place.
                const std::uint32_t core_w = core_x1 - core_x0;
                const std::uint32_t core_h = core_y1 - core_y0;
                const std::uint32_t crop_x0 = (core_x0 - pad_x0) * kScale;
                const std::uint32_t crop_y0 = (core_y0 - pad_y0) * kScale;
                const std::uint32_t crop_w = core_w * kScale;
                const std::uint32_t crop_h = core_h * kScale;
                const std::uint32_t dest_w = core_w * factor;
                const std::uint32_t dest_h = core_h * factor;
                const std::uint32_t dest_x0 = core_x0 * factor;
                const std::uint32_t dest_y0 = core_y0 * factor;

                for (std::uint32_t y = 0; y < dest_h; ++y) {
                    const float fy = (static_cast<float>(y) + 0.5f) * crop_h / dest_h - 0.5f
                            + static_cast<float>(crop_y0);
                    for (std::uint32_t x = 0; x < dest_w; ++x) {
                        const float fx = (static_cast<float>(x) + 0.5f) * crop_w / dest_w - 0.5f
                                + static_cast<float>(crop_x0);
                        const std::size_t out_at =
                                (static_cast<std::size_t>(dest_y0 + y) * out_w + dest_x0 + x) * 3;
                        for (std::uint32_t c = 0; c < 3; ++c) {
                            const float v = sample_plane(
                                    data + static_cast<std::size_t>(c) * model_h * model_w, model_w,
                                    model_h, fx, fy);
                            out[out_at + c] = static_cast<std::uint8_t>(
                                    clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f);
                        }
                    }
                }

                ++tile_index;
                if (progress) {
                    progress(static_cast<double>(tile_index) / total_tiles, "enhancing");
                }
            }
        }
    } catch (const Ort::Exception &e) {
        outcome.error = "inference failed: " + std::string(e.what());
        return outcome;
    }

    outcome.ok = true;
    outcome.width = out_w;
    outcome.height = out_h;
    outcome.rgb = std::move(out);
    if (progress) {
        progress(1.0, "done");
    }
    return outcome;
}

EnhanceProvider::EnhanceProvider() = default;
EnhanceProvider::EnhanceProvider(EnhanceProvider &&) noexcept = default;
EnhanceProvider &EnhanceProvider::operator=(EnhanceProvider &&) noexcept = default;
EnhanceProvider::~EnhanceProvider() = default;

} // namespace genesis::ai::vision

#else // GENESIS_AI_VISION_ENABLED not defined: the runtime-free build.

namespace genesis::ai::vision {

// The pimpl type must be complete for the unique_ptr member's destructor, even
// in the runtime-free build, so it is a harmless empty struct here.
struct EnhanceProvider::Impl
{
};

bool EnhanceProvider::available()
{
    return false;
}

std::string_view EnhanceProvider::status()
{
    return "disabled";
}

EnhanceProvider EnhanceProvider::load(std::string_view)
{
    EnhanceProvider provider;
    provider.error_ = "disabled: ONNX Runtime not compiled in "
                      "(rebuild with GENESIS_AI_VISION=ON)";
    return provider;
}

EnhanceOutcome EnhanceProvider::enhance(const FrameView &, std::uint32_t, const ProgressFn &,
                                        const AbortFn &) const
{
    EnhanceOutcome outcome;
    outcome.error = error_.empty() ? "disabled: ONNX Runtime not compiled in "
                                     "(rebuild with GENESIS_AI_VISION=ON)"
                                   : error_;
    return outcome;
}

EnhanceProvider::EnhanceProvider() = default;
EnhanceProvider::EnhanceProvider(EnhanceProvider &&) noexcept = default;
EnhanceProvider &EnhanceProvider::operator=(EnhanceProvider &&) noexcept = default;
EnhanceProvider::~EnhanceProvider() = default;

} // namespace genesis::ai::vision

#endif // GENESIS_AI_VISION_ENABLED
