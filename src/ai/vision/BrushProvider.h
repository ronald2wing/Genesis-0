// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ai/vision/MaskDriver.h"

namespace genesis::ai::vision {

// The encoder's fixed input resolution: every frame is resized so its long
// side is 1024 and padded to a 1024x1024 square before it feeds the vision
// encoder (SlimSAM's patch embedding is a 16x16-stride-16 convolution, so the
// input must be square and a multiple of 16).
constexpr std::uint32_t brush_model_size = 1024;

// How a WxH frame maps into the encoder's 1024-long-side input space. The
// frame is resized to (resized_width, resized_height) - aspect preserved, long
// side = brush_model_size - and the rest of the square is zero-padded.
struct BrushInputSize
{
    // The resize scale: brush_model_size / max(W, H). 1.0 for a square frame.
    double scale = 1.0;
    // The frame's size after the long-side resize (aspect preserved).
    std::uint32_t resized_width = 0;
    std::uint32_t resized_height = 0;
};

// The resize that maps a frame onto the encoder's input. Pure arithmetic,
// compiled in both builds; a zero-sized frame maps to a zero resized size
// (refine() refuses an empty frame before this is ever consulted).
BrushInputSize brush_input_size(std::uint32_t width, std::uint32_t height);

// Blends one stroke's refined mask (in [0,1], width*height) into the base
// alpha (same shape) for that stroke's tool: a keep tool takes the union
// (alpha = max(alpha, refined)), a drop tool subtracts (alpha = alpha *
// (1 - refined)). Only the two smart tools blend a model refinement, so a
// plain brush/eraser is a no-op here - those paint a disc, not a model result.
// Pure arithmetic, compiled in both builds; the two vectors must be the same
// length (a mismatch is a no-op).
void brush_combine(std::vector<float> &alpha, const std::vector<float> &refined,
                   genesis::project::BrushTool tool);

// The ONNX smart-brush provider (SlimSAM), engine-free and Qt-free. It refines
// a base alpha mask with the smart strokes painted at one frame: the vision
// encoder embeds the frame once (cached by frame), and the prompt
// encoder/mask decoder turns each stroke's points into a correction blended
// over the base per its tool. The two models (encoder, decoder) are loaded
// once (load) and their sessions reused across refine() calls.
//
// The models are the pinned Xenova/slimsam-77-uniform transformers.js export:
// the encoder takes `pixel_values` [1,3,1024,1024] (with the caller applying
// the ImageNet normalization) and yields `image_embeddings` and
// `image_positional_embeddings` ([1,256,64,64] each); the decoder takes
// `input_points` [1,1,N,2] float, `input_labels` [1,1,N] int64, and the two
// embeddings, and yields `iou_scores` [1,1,3] and `pred_masks`
// [1,1,3,256,256] (logits, low-res). Points ride in the 1024-long-side frame
// coordinate space; the low-res mask is cropped to the frame's top-left region
// and resized back to the frame. A positive label (SmartBrush) keeps, a
// negative label (SmartEraser) drops.
//
// When ONNX Runtime was not compiled in (the default build), available()
// returns false, status() reports "disabled", and load() yields a provider
// whose ok() is false with a "disabled" reason - the caller learns this at
// runtime rather than failing to link.
class BrushProvider
{
public:
    // Whether ONNX Runtime was compiled in. False in the default build
    // (GENESIS_AI_VISION=OFF).
    static bool available();

    // "enabled" or "disabled": the runtime state, for logs and UI.
    static std::string_view status();

    // Loads the encoder and decoder models. Returns a provider with
    // ok()==false and a non-empty error() when the runtime is disabled or
    // either model fails to load. Never throws.
    static BrushProvider load(std::string_view encoder_path, std::string_view decoder_path);

    // Whether both models loaded. False for a default-constructed provider.
    bool ok() const { return impl_ != nullptr; }
    // Why load() failed; empty when ok().
    const std::string &error() const { return error_; }

    // Refines `alpha` (width*height floats in [0,1], row-major) in place for
    // the strokes at one frame. Returns false with a reason when the runtime
    // is disabled, the models failed to load, the frame is empty, the alpha
    // plane does not match the frame, or the inference fails; on failure
    // `alpha` is left untouched, so the caller keeps the base alpha (a refine
    // failure is non-fatal). The strokes are applied in order, each blended
    // per its tool; a non-smart stroke or one with no points is skipped.
    bool refine(std::vector<float> &alpha, const FrameView &frame,
                const std::vector<BrushStroke> &strokes, std::string &error);

    BrushProvider();
    BrushProvider(BrushProvider &&) noexcept;
    BrushProvider &operator=(BrushProvider &&) noexcept;
    ~BrushProvider();

    BrushProvider(const BrushProvider &) = delete;
    BrushProvider &operator=(const BrushProvider &) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string error_;
};

} // namespace genesis::ai::vision
