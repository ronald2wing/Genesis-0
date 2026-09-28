// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "ai/vision/SegmentationProvider.h"

#if defined(GENESIS_AI_VISION_ENABLED)

#  include "ai/vision/ExecutionProvider.h"

#  include <algorithm>
#  include <array>
#  include <cmath>
#  include <cstddef>
#  include <cstdint>
#  include <cstring>
#  include <memory>
#  include <string>
#  include <utility>
#  include <vector>

#  include <onnxruntime_cxx_api.h>

namespace genesis::ai::vision {

namespace {

// The fixed input resolution each model is preprocessed to. RVM runs at
// 512x512 (its recurrent states are sized from this), IS-Net at 1024x1024 (the
// only shape its ONNX graph accepts). The frame is bilinearly resized to this,
// run, and the mask resized back to the frame's own size.
constexpr std::uint32_t kRvmSize = 512;
constexpr std::uint32_t kIsNetSize = 1024;

// RVM's recurrent hidden-state channels, in decoder order (r1..r4). Pinned from
// the model's RecurrentDecoder([16,24,40,128], [80,40,32,16]) - the GRU hidden
// sizes are 16/20/40/64 (see PeterL1n/RobustVideoMatting model/decoder.py).
constexpr std::array<std::uint32_t, 4> kRvmRecChannels = { 16, 20, 40, 64 };

float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

int clamp_int(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Bilinear sample of the interleaved RGB frame at continuous (x, y) in frame
// coordinates, clamped at the edges. Writes three values in 0..255.
void sample_rgb(const FrameView &frame, float x, float y, float out[3])
{
    const int w = static_cast<int>(frame.width);
    const int h = static_cast<int>(frame.height);
    const int x0 = clamp_int(static_cast<int>(std::floor(x)), 0, w - 1);
    const int y0 = clamp_int(static_cast<int>(std::floor(y)), 0, h - 1);
    const int x1 = clamp_int(x0 + 1, 0, w - 1);
    const int y1 = clamp_int(y0 + 1, 0, h - 1);
    const float tx = clampf(x - static_cast<float>(x0), 0.0f, 1.0f);
    const float ty = clampf(y - static_cast<float>(y0), 0.0f, 1.0f);
    for (int c = 0; c < 3; ++c) {
        const float a = frame.rgb[(y0 * w + x0) * 3 + c];
        const float b = frame.rgb[(y0 * w + x1) * 3 + c];
        const float d = frame.rgb[(y1 * w + x0) * 3 + c];
        const float e = frame.rgb[(y1 * w + x1) * 3 + c];
        out[c] = (a * (1.0f - tx) + b * tx) * (1.0f - ty) + (d * (1.0f - tx) + e * tx) * ty;
    }
}

// Resizes the frame to (mw, mh) and emits it as a CHW float tensor, applying
// `value * scale + shift` per channel. RVM: scale 1/255, shift 0 (plain
// 0..1). IS-Net: scale 1/256, shift -128/256 (its ViT-style normalization).
std::vector<float> to_tensor(const FrameView &frame, std::uint32_t mw, std::uint32_t mh,
                             float scale, float shift)
{
    std::vector<float> out(3ull * mw * mh);
    for (std::uint32_t y = 0; y < mh; ++y) {
        const float fy = (static_cast<float>(y) + 0.5f) * frame.height / mh - 0.5f;
        for (std::uint32_t x = 0; x < mw; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f) * frame.width / mw - 0.5f;
            float rgb[3];
            sample_rgb(frame, fx, fy, rgb);
            out[0 * mw * mh + static_cast<std::size_t>(y) * mw + x] = rgb[0] * scale + shift;
            out[1 * mw * mh + static_cast<std::size_t>(y) * mw + x] = rgb[1] * scale + shift;
            out[2 * mw * mh + static_cast<std::size_t>(y) * mw + x] = rgb[2] * scale + shift;
        }
    }
    return out;
}

// Bilinear sample of a single plane (row-major, sw x sh).
float sample_plane(const float *src, std::uint32_t sw, std::uint32_t sh, float x, float y)
{
    const int w = static_cast<int>(sw);
    const int h = static_cast<int>(sh);
    const int x0 = clamp_int(static_cast<int>(std::floor(x)), 0, w - 1);
    const int y0 = clamp_int(static_cast<int>(std::floor(y)), 0, h - 1);
    const int x1 = clamp_int(x0 + 1, 0, w - 1);
    const int y1 = clamp_int(y0 + 1, 0, h - 1);
    const float tx = clampf(x - static_cast<float>(x0), 0.0f, 1.0f);
    const float ty = clampf(y - static_cast<float>(y0), 0.0f, 1.0f);
    const float a = src[y0 * w + x0];
    const float b = src[y0 * w + x1];
    const float d = src[y1 * w + x0];
    const float e = src[y1 * w + x1];
    return (a * (1.0f - tx) + b * tx) * (1.0f - ty) + (d * (1.0f - tx) + e * tx) * ty;
}

// Resizes a model-size alpha plane back to the frame's own size.
std::vector<float> resize_alpha(const std::vector<float> &src, std::uint32_t sw, std::uint32_t sh,
                                std::uint32_t dw, std::uint32_t dh)
{
    std::vector<float> out(static_cast<std::size_t>(dw) * dh);
    for (std::uint32_t y = 0; y < dh; ++y) {
        const float fy = (static_cast<float>(y) + 0.5f) * sh / dh - 0.5f;
        for (std::uint32_t x = 0; x < dw; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f) * sw / dw - 0.5f;
            out[static_cast<std::size_t>(y) * dw + x] =
                    clampf(sample_plane(src.data(), sw, sh, fx, fy), 0.0f, 1.0f);
        }
    }
    return out;
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

struct SegmentationProvider::Impl
{
    SegmentModel model = SegmentModel::Rvm;
    std::uint32_t model_w = 0;
    std::uint32_t model_h = 0;
    Ort::Env env{ nullptr };
    Ort::Session session{ nullptr };
    std::vector<std::string> input_names;
    std::vector<std::string> output_names;
};

bool SegmentationProvider::available()
{
    return true;
}

std::string_view SegmentationProvider::status()
{
    return "enabled";
}

SegmentationProvider SegmentationProvider::load(std::string_view model_path, SegmentModel model)
{
    SegmentationProvider provider;
    if (model_path.empty()) {
        provider.error_ = "no model path";
        return provider;
    }

    auto impl = std::make_unique<Impl>();
    impl->model = model;
    impl->model_w = model == SegmentModel::Rvm ? kRvmSize : kIsNetSize;
    impl->model_h = impl->model_w;

    try {
        impl->env = Ort::Env(ORT_LOGGING_LEVEL_WARNING, "genesis-ai-vision");
        const std::string path(model_path);
        impl->session = genesis::ai::make_session(impl->env, path.c_str());
    } catch (const Ort::Exception &e) {
        provider.error_ = "failed to load model: " + std::string(e.what());
        return provider;
    }

    // Record the input/output names this exact graph expects, so segment()
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

    provider.impl_ = std::move(impl);
    return provider;
}

SegmentationOutcome SegmentationProvider::segment(const FrameView &frame,
                                                  const ProgressFn &progress,
                                                  const AbortFn &abort) const
{
    SegmentationOutcome outcome;
    if (impl_ == nullptr) {
        outcome.error = error_.empty() ? "not loaded" : error_;
        return outcome;
    }
    if (frame.rgb == nullptr || frame.width == 0 || frame.height == 0) {
        outcome.error = "empty frame";
        return outcome;
    }

    if (progress) {
        progress(0.0, "preprocessing");
    }
    if (abort && abort()) {
        outcome.error = "cancelled";
        return outcome;
    }

    const std::uint32_t mw = impl_->model_w;
    const std::uint32_t mh = impl_->model_h;
    const bool rvm = impl_->model == SegmentModel::Rvm;
    const float scale = rvm ? 1.0f / 255.0f : 1.0f / 256.0f;
    const float shift = rvm ? 0.0f : -128.0f / 256.0f;
    std::vector<float> src = to_tensor(frame, mw, mh, scale, shift);

    if (progress) {
        progress(0.4, "segmenting");
    }
    if (abort && abort()) {
        outcome.error = "cancelled";
        return outcome;
    }

    Ort::MemoryInfo info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    try {
        std::vector<Ort::Value> inputs;
        std::vector<const char *> input_ptrs;
        input_ptrs.reserve(impl_->input_names.size());
        for (const std::string &name : impl_->input_names) {
            input_ptrs.push_back(name.c_str());
        }

        // The recurrent-state and downsample-ratio buffers live here, not
        // inside the rvm branch: CreateTensor wraps a caller-owned pointer
        // rather than copying, so they must stay alive through the Run below.
        std::vector<std::vector<float>> rec;
        std::vector<float> ratio(1, 0.25f);

        if (rvm) {
            // RVM: src + four zero recurrent states + the downsample ratio.
            // A per-frame call zero-initializes the recurrence, so each frame
            // is matted independently (no cross-frame smoothing; a
            // sequence-aware API is out of scope for the frame seam).
            std::vector<Ort::Value> rec_tensors;
            rec.reserve(4);
            rec_tensors.reserve(4);
            for (std::size_t i = 0; i < kRvmRecChannels.size(); ++i) {
                const std::uint32_t rh = mh >> (3 + i); // /8, /16, /32, /64
                const std::uint32_t rw = mw >> (3 + i);
                rec.emplace_back(static_cast<std::size_t>(kRvmRecChannels[i]) * rh * rw, 0.0f);
                rec_tensors.push_back(
                        tensor(info, { 1, static_cast<std::int64_t>(kRvmRecChannels[i]), rh, rw },
                               rec.back().data()));
            }
            Ort::Value ratio_tensor = tensor(info, { 1 }, ratio.data());

            // Input order must match the model's input order, not a guess.
            for (std::size_t i = 0; i < impl_->input_names.size(); ++i) {
                if (impl_->input_names[i] == "src") {
                    inputs.push_back(tensor(info, { 1, 3, mh, mw }, src.data()));
                } else if (impl_->input_names[i] == "r1i") {
                    inputs.push_back(std::move(rec_tensors[0]));
                } else if (impl_->input_names[i] == "r2i") {
                    inputs.push_back(std::move(rec_tensors[1]));
                } else if (impl_->input_names[i] == "r3i") {
                    inputs.push_back(std::move(rec_tensors[2]));
                } else if (impl_->input_names[i] == "r4i") {
                    inputs.push_back(std::move(rec_tensors[3]));
                } else if (impl_->input_names[i] == "downsample_ratio") {
                    inputs.push_back(std::move(ratio_tensor));
                } else {
                    outcome.error = "unexpected RVM input: " + impl_->input_names[i];
                    return outcome;
                }
            }
        } else {
            // IS-Net: a single normalized image tensor.
            if (impl_->input_names.size() != 1) {
                outcome.error = "unexpected IS-Net input count";
                return outcome;
            }
            inputs.push_back(tensor(info, { 1, 3, mh, mw }, src.data()));
        }

        // Request only the mask output; the recurrent outputs (RVM) are
        // computed internally and discarded - a per-frame call has no next
        // frame to feed them to.
        const std::string mask_output = rvm ? "pha" : "output";
        std::vector<const char *> output_ptrs{ mask_output.c_str() };
        std::vector<Ort::Value> outputs =
                impl_->session.Run(Ort::RunOptions{ nullptr }, input_ptrs.data(), inputs.data(),
                                   inputs.size(), output_ptrs.data(), 1);

        const Ort::Value &mask = outputs[0];
        const auto shape = mask.GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() != 4) {
            outcome.error = "unexpected mask output rank";
            return outcome;
        }
        const float *data = mask.GetTensorData<float>();
        const std::uint32_t out_h = static_cast<std::uint32_t>(shape[2]);
        const std::uint32_t out_w = static_cast<std::uint32_t>(shape[3]);
        std::vector<float> plane(data, data + static_cast<std::size_t>(out_w) * out_h);

        outcome.alpha = resize_alpha(plane, out_w, out_h, frame.width, frame.height);
    } catch (const Ort::Exception &e) {
        outcome.error = "inference failed: " + std::string(e.what());
        return outcome;
    }

    if (progress) {
        progress(1.0, "done");
    }
    outcome.ok = true;
    outcome.width = frame.width;
    outcome.height = frame.height;
    return outcome;
}

SegmentationProvider::SegmentationProvider() = default;
SegmentationProvider::SegmentationProvider(SegmentationProvider &&) noexcept = default;
SegmentationProvider &SegmentationProvider::operator=(SegmentationProvider &&) noexcept = default;
SegmentationProvider::~SegmentationProvider() = default;

} // namespace genesis::ai::vision

#else // GENESIS_AI_VISION_ENABLED not defined: the runtime-free build.

namespace genesis::ai::vision {

// The pimpl type must be complete for the unique_ptr member's destructor, even
// in the runtime-free build, so it is a harmless empty struct here.
struct SegmentationProvider::Impl
{
};

bool SegmentationProvider::available()
{
    return false;
}

std::string_view SegmentationProvider::status()
{
    return "disabled";
}

SegmentationProvider SegmentationProvider::load(std::string_view, SegmentModel)
{
    SegmentationProvider provider;
    provider.error_ = "disabled: ONNX Runtime not compiled in "
                      "(rebuild with GENESIS_AI_VISION=ON)";
    return provider;
}

SegmentationOutcome SegmentationProvider::segment(const FrameView &, const ProgressFn &,
                                                  const AbortFn &) const
{
    SegmentationOutcome outcome;
    outcome.error = error_.empty() ? "disabled: ONNX Runtime not compiled in "
                                     "(rebuild with GENESIS_AI_VISION=ON)"
                                   : error_;
    return outcome;
}

SegmentationProvider::SegmentationProvider() = default;
SegmentationProvider::SegmentationProvider(SegmentationProvider &&) noexcept = default;
SegmentationProvider &SegmentationProvider::operator=(SegmentationProvider &&) noexcept = default;
SegmentationProvider::~SegmentationProvider() = default;

} // namespace genesis::ai::vision

#endif // GENESIS_AI_VISION_ENABLED
