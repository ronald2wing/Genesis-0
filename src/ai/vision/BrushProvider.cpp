// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "ai/vision/BrushProvider.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace genesis::ai::vision {

// Shared by both builds: pure arithmetic helpers the driver and controller can
// call before a run without a runtime, so the stroke mapping and the blend
// semantics stay testable in the default build (see BrushGatingTests).

BrushInputSize brush_input_size(std::uint32_t width, std::uint32_t height)
{
    BrushInputSize out;
    if (width == 0 || height == 0) {
        return out; // a zero-sized frame maps to a zero resized size
    }
    const double longest = static_cast<double>(width > height ? width : height);
    out.scale = static_cast<double>(brush_model_size) / longest;
    out.resized_width =
            static_cast<std::uint32_t>(std::lround(static_cast<double>(width) * out.scale));
    out.resized_height =
            static_cast<std::uint32_t>(std::lround(static_cast<double>(height) * out.scale));
    return out;
}

void brush_combine(std::vector<float> &alpha, const std::vector<float> &refined,
                   genesis::project::BrushTool tool)
{
    if (alpha.size() != refined.size()) {
        return; // a shape mismatch is a no-op, never a crash
    }
    const bool keep = tool == genesis::project::BrushTool::SmartBrush;
    const bool drop = tool == genesis::project::BrushTool::SmartEraser;
    if (!keep && !drop) {
        return; // a plain brush/eraser paints a disc, not a model result
    }
    for (std::size_t i = 0; i < alpha.size(); ++i) {
        if (keep) {
            alpha[i] = std::max(alpha[i], refined[i]);
        } else {
            alpha[i] = alpha[i] * (1.0f - refined[i]);
        }
    }
}

} // namespace genesis::ai::vision

#if defined(GENESIS_AI_VISION_ENABLED)

#  include "ai/vision/ExecutionProvider.h"

#  include <cstddef>
#  include <memory>
#  include <string>
#  include <string_view>
#  include <utility>

#  include <onnxruntime_cxx_api.h>

namespace genesis::ai::vision {

namespace {

// The low-res mask the decoder yields is a fixed quarter of the encoder input:
// 1024 / 4 = 256. The frame (resized to the long side, aspect preserved) maps
// to the top-left of that plane.
constexpr std::uint32_t kLowRes = 256;

// ImageNet mean/std the caller applies before the encoder: SlimSAM's patch
// embedding consumes raw pixels, so the normalization is the provider's job.
constexpr float kMean[3] = { 0.485f, 0.456f, 0.406f };
constexpr float kStd[3] = { 0.229f, 0.224f, 0.225f };

float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

int clamp_int(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

float sigmoidf(float v)
{
    return 1.0f / (1.0f + std::exp(-v));
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

// Builds the encoder input: the frame bilinearly resized so its long side is
// 1024 (aspect preserved), zero-padded right/bottom to the 1024x1024 square,
// then ImageNet-normalized per channel. The pad is raw pixel 0, so a padded
// pixel normalizes to -mean/std (the same zero the export pads with).
std::vector<float> encoder_tensor(const FrameView &frame, const BrushInputSize &size)
{
    const std::uint32_t mw = brush_model_size;
    const std::uint32_t mh = brush_model_size;
    std::vector<float> out(3ull * mw * mh);
    for (std::uint32_t y = 0; y < mh; ++y) {
        for (std::uint32_t x = 0; x < mw; ++x) {
            float rgb[3];
            if (x < size.resized_width && y < size.resized_height) {
                // The resized frame occupies the top-left; sample the source at
                // the pixel the encoder's own resize would have read.
                const float fx = (static_cast<float>(x) + 0.5f) * frame.width
                                / static_cast<float>(size.resized_width)
                        - 0.5f;
                const float fy = (static_cast<float>(y) + 0.5f) * frame.height
                                / static_cast<float>(size.resized_height)
                        - 0.5f;
                sample_rgb(frame, fx, fy, rgb);
                rgb[0] /= 255.0f;
                rgb[1] /= 255.0f;
                rgb[2] /= 255.0f;
            } else {
                rgb[0] = rgb[1] = rgb[2] = 0.0f;
            }
            const std::size_t at = static_cast<std::size_t>(y) * mw + x;
            out[at] = (rgb[0] - kMean[0]) / kStd[0];
            out[at + static_cast<std::size_t>(mw) * mh] = (rgb[1] - kMean[1]) / kStd[1];
            out[at + 2 * static_cast<std::size_t>(mw) * mh] = (rgb[2] - kMean[2]) / kStd[2];
        }
    }
    return out;
}

// Bilinear sample of one plane (row-major, sw x sh).
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

// Resizes a low-res alpha plane (row-major, sw x sh) back to the frame's size.
std::vector<float> resize_plane(const std::vector<float> &src, std::uint32_t sw, std::uint32_t sh,
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

// The embeddings shape both encoder outputs and both decoder inputs share.
const std::vector<std::int64_t> &embedding_shape()
{
    static const std::vector<std::int64_t> shape = { 1, 256, 64, 64 };
    return shape;
}

} // namespace

struct BrushProvider::Impl
{
    Ort::Env env{ nullptr };
    Ort::Session encoder{ nullptr };
    Ort::Session decoder{ nullptr };
    std::vector<std::string> encoder_input_names;
    std::vector<std::string> encoder_output_names;
    std::vector<std::string> decoder_input_names;
    std::vector<std::string> decoder_output_names;
    // Single-slot embedding cache keyed by the frame index a refine() names:
    // the encoder is the expensive half, and a frame's strokes are applied in
    // one refine() call, so the next call for the same frame reuses the run.
    std::uint32_t cached_frame = 0;
    bool cached = false;
    std::vector<float> image_embeddings;
    std::vector<float> image_positional_embeddings;
};

bool BrushProvider::available()
{
    return true;
}

std::string_view BrushProvider::status()
{
    return "enabled";
}

BrushProvider BrushProvider::load(std::string_view encoder_path, std::string_view decoder_path)
{
    BrushProvider provider;
    if (encoder_path.empty() || decoder_path.empty()) {
        provider.error_ = "no model path";
        return provider;
    }

    auto impl = std::make_unique<Impl>();
    try {
        impl->env = Ort::Env(ORT_LOGGING_LEVEL_WARNING, "genesis-ai-brush");
        const std::string encoder(encoder_path);
        const std::string decoder(decoder_path);
        impl->encoder = genesis::ai::make_session(impl->env, encoder.c_str());
        impl->decoder = genesis::ai::make_session(impl->env, decoder.c_str());
    } catch (const Ort::Exception &e) {
        provider.error_ = "failed to load model: " + std::string(e.what());
        return provider;
    }

    // Record the input/output names each graph expects, so refine() feeds them
    // in the model's own order rather than assuming a fixed one.
    Ort::AllocatorWithDefaultOptions allocator;
    const auto record = [&](Ort::Session &session, std::vector<std::string> &inputs,
                            std::vector<std::string> &outputs) {
        inputs.reserve(session.GetInputCount());
        for (std::size_t i = 0; i < session.GetInputCount(); ++i) {
            inputs.push_back(session.GetInputNameAllocated(i, allocator).get());
        }
        outputs.reserve(session.GetOutputCount());
        for (std::size_t i = 0; i < session.GetOutputCount(); ++i) {
            outputs.push_back(session.GetOutputNameAllocated(i, allocator).get());
        }
    };
    record(impl->encoder, impl->encoder_input_names, impl->encoder_output_names);
    record(impl->decoder, impl->decoder_input_names, impl->decoder_output_names);

    provider.impl_ = std::move(impl);
    return provider;
}

bool BrushProvider::refine(std::vector<float> &alpha, const FrameView &frame,
                           const std::vector<BrushStroke> &strokes, std::string &error)
{
    if (impl_ == nullptr) {
        error = error_.empty() ? "not loaded" : error_;
        return false;
    }
    if (frame.rgb == nullptr || frame.width == 0 || frame.height == 0) {
        error = "empty frame";
        return false;
    }
    const std::size_t pixel_count = static_cast<std::size_t>(frame.width) * frame.height;
    if (alpha.size() != pixel_count) {
        error = "alpha does not match frame";
        return false;
    }

    // Refine a local copy and commit only when every stroke succeeds: a refine
    // failure leaves `alpha` untouched, so the caller keeps the base alpha.
    std::vector<float> result = alpha;

    const BrushInputSize size = brush_input_size(frame.width, frame.height);
    const Ort::MemoryInfo info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    // Embed the frame once per refine() call (cached by frame index), then turn
    // each smart stroke into a correction blended per its tool.
    for (const BrushStroke &stroke : strokes) {
        const bool keep = stroke.tool == genesis::project::BrushTool::SmartBrush;
        const bool drop = stroke.tool == genesis::project::BrushTool::SmartEraser;
        if ((!keep && !drop) || stroke.points.empty()) {
            continue; // a plain disc, or nothing to read the model from
        }

        try {
            if (!impl_->cached || impl_->cached_frame != stroke.frame) {
                std::vector<float> src = encoder_tensor(frame, size);
                std::vector<const char *> enc_in_ptrs{ impl_->encoder_input_names[0].c_str() };
                std::vector<const char *> enc_out_ptrs;
                enc_out_ptrs.reserve(impl_->encoder_output_names.size());
                for (const std::string &name : impl_->encoder_output_names) {
                    enc_out_ptrs.push_back(name.c_str());
                }
                std::vector<Ort::Value> enc_inputs;
                enc_inputs.push_back(
                        tensor(info, { 1, 3, brush_model_size, brush_model_size }, src.data()));
                std::vector<Ort::Value> enc_outputs = impl_->encoder.Run(
                        Ort::RunOptions{ nullptr }, enc_in_ptrs.data(), enc_inputs.data(),
                        enc_inputs.size(), enc_out_ptrs.data(), enc_out_ptrs.size());
                for (std::size_t i = 0; i < impl_->encoder_output_names.size(); ++i) {
                    const float *data = enc_outputs[i].GetTensorData<float>();
                    const std::size_t n =
                            enc_outputs[i].GetTensorTypeAndShapeInfo().GetElementCount();
                    std::vector<float> plane(data, data + n);
                    if (impl_->encoder_output_names[i] == "image_embeddings") {
                        impl_->image_embeddings = std::move(plane);
                    } else if (impl_->encoder_output_names[i] == "image_positional_embeddings") {
                        impl_->image_positional_embeddings = std::move(plane);
                    }
                }
                impl_->cached_frame = stroke.frame;
                impl_->cached = true;
            }

            // The points ride in the resized (1024-long-side) coordinate space:
            // a source fraction maps to fraction * resized side. The prompt
            // encoder's +0.5 centering and padding are inside the graph, so the
            // provider passes raw coordinates.
            const std::size_t n_pts = stroke.points.size();
            std::vector<float> points(n_pts * 2);
            std::vector<std::int64_t> labels(n_pts, keep ? 1 : 0);
            for (std::size_t i = 0; i < n_pts; ++i) {
                points[i * 2] = static_cast<float>(stroke.points[i][0] * size.resized_width);
                points[i * 2 + 1] = static_cast<float>(stroke.points[i][1] * size.resized_height);
            }
            Ort::Value points_tensor = Ort::Value::CreateTensor<float>(
                    info, points.data(), points.size(),
                    std::vector<std::int64_t>{ 1, 1, static_cast<std::int64_t>(n_pts), 2 }.data(),
                    4);
            Ort::Value labels_tensor = Ort::Value::CreateTensor<std::int64_t>(
                    info, labels.data(), labels.size(),
                    std::vector<std::int64_t>{ 1, 1, static_cast<std::int64_t>(n_pts) }.data(), 3);

            const std::vector<std::int64_t> &emb_shape = embedding_shape();
            Ort::Value emb_tensor = tensor(info, emb_shape, impl_->image_embeddings.data());
            Ort::Value pe_tensor =
                    tensor(info, emb_shape, impl_->image_positional_embeddings.data());

            std::vector<Ort::Value> dec_inputs;
            std::vector<const char *> dec_in_ptrs;
            dec_inputs.reserve(impl_->decoder_input_names.size());
            dec_in_ptrs.reserve(impl_->decoder_input_names.size());
            for (const std::string &name : impl_->decoder_input_names) {
                dec_in_ptrs.push_back(name.c_str());
                if (name == "input_points") {
                    dec_inputs.push_back(std::move(points_tensor));
                } else if (name == "input_labels") {
                    dec_inputs.push_back(std::move(labels_tensor));
                } else if (name == "image_embeddings") {
                    dec_inputs.push_back(std::move(emb_tensor));
                } else if (name == "image_positional_embeddings") {
                    dec_inputs.push_back(std::move(pe_tensor));
                } else {
                    error = "unexpected decoder input: " + name;
                    return false;
                }
            }

            std::vector<const char *> dec_out_ptrs;
            dec_out_ptrs.reserve(impl_->decoder_output_names.size());
            for (const std::string &name : impl_->decoder_output_names) {
                dec_out_ptrs.push_back(name.c_str());
            }
            std::vector<Ort::Value> dec_outputs = impl_->decoder.Run(
                    Ort::RunOptions{ nullptr }, dec_in_ptrs.data(), dec_inputs.data(),
                    dec_inputs.size(), dec_out_ptrs.data(), dec_out_ptrs.size());

            const float *iou = nullptr;
            const float *masks = nullptr;
            for (std::size_t i = 0; i < impl_->decoder_output_names.size(); ++i) {
                if (impl_->decoder_output_names[i] == "iou_scores") {
                    iou = dec_outputs[i].GetTensorData<float>();
                } else if (impl_->decoder_output_names[i] == "pred_masks") {
                    masks = dec_outputs[i].GetTensorData<float>();
                }
            }
            if (iou == nullptr || masks == nullptr) {
                error = "decoder output missing";
                return false;
            }

            // One prompt yields three candidate masks; keep the one the model
            // scored highest, sigmoid its logits, crop the frame's top-left
            // region of the low-res plane, and resize back to the frame.
            int best = 0;
            if (iou[1] > iou[best]) {
                best = 1;
            }
            if (iou[2] > iou[best]) {
                best = 2;
            }
            const std::uint32_t low_w = (size.resized_width + 3) / 4;
            const std::uint32_t low_h = (size.resized_height + 3) / 4;
            const float *logits = masks + static_cast<std::size_t>(best) * kLowRes * kLowRes;
            std::vector<float> plane(static_cast<std::size_t>(low_w) * low_h);
            for (std::uint32_t y = 0; y < low_h; ++y) {
                for (std::uint32_t x = 0; x < low_w; ++x) {
                    plane[static_cast<std::size_t>(y) * low_w + x] =
                            sigmoidf(logits[static_cast<std::size_t>(y) * kLowRes + x]);
                }
            }
            std::vector<float> refined =
                    resize_plane(plane, low_w, low_h, frame.width, frame.height);
            brush_combine(result, refined, stroke.tool);
        } catch (const Ort::Exception &e) {
            error = "inference failed: " + std::string(e.what());
            return false;
        }
    }

    alpha = std::move(result);
    return true;
}

BrushProvider::BrushProvider() = default;
BrushProvider::BrushProvider(BrushProvider &&) noexcept = default;
BrushProvider &BrushProvider::operator=(BrushProvider &&) noexcept = default;
BrushProvider::~BrushProvider() = default;

} // namespace genesis::ai::vision

#else // GENESIS_AI_VISION_ENABLED not defined: the runtime-free build.

#  include <string>
#  include <string_view>

namespace genesis::ai::vision {

// The pimpl type must be complete for the unique_ptr member's destructor, even
// in the runtime-free build, so it is a harmless empty struct here.
struct BrushProvider::Impl
{
};

bool BrushProvider::available()
{
    return false;
}

std::string_view BrushProvider::status()
{
    return "disabled";
}

BrushProvider BrushProvider::load(std::string_view, std::string_view)
{
    BrushProvider provider;
    provider.error_ = "disabled: ONNX Runtime not compiled in "
                      "(rebuild with GENESIS_AI_VISION=ON)";
    return provider;
}

bool BrushProvider::refine(std::vector<float> &, const FrameView &,
                           const std::vector<BrushStroke> &, std::string &error)
{
    error = error_.empty() ? "disabled: ONNX Runtime not compiled in "
                             "(rebuild with GENESIS_AI_VISION=ON)"
                           : error_;
    return false;
}

BrushProvider::BrushProvider() = default;
BrushProvider::BrushProvider(BrushProvider &&) noexcept = default;
BrushProvider &BrushProvider::operator=(BrushProvider &&) noexcept = default;
BrushProvider::~BrushProvider() = default;

} // namespace genesis::ai::vision

#endif // GENESIS_AI_VISION_ENABLED
