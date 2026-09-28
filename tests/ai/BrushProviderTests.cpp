// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "ai/vision/BrushProvider.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

namespace vision = genesis::ai::vision;
using Tool = genesis::project::BrushTool;

// The models to run: GENESIS_BRUSH_ENCODER_MODEL / GENESIS_BRUSH_DECODER_MODEL
// name the installed .onnx files, else empty. ctest never downloads a model; a
// build without both set skips the run cleanly.
std::string model_path(const char *env)
{
    const char *value = std::getenv(env);
    if (value == nullptr || *value == '\0') {
        return { };
    }
    return value;
}

// A synthetic RGB frame: a vertical gradient plus a bright centre block, so the
// encoder sees real content rather than a flat buffer. The assertions only
// cover well-formedness - never a specific mask - because the models are
// content.
std::vector<std::uint8_t> make_frame(std::uint32_t w, std::uint32_t h)
{
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(w) * h * 3);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t r = static_cast<std::uint8_t>((x * 255) / w);
            std::uint8_t g = static_cast<std::uint8_t>((y * 255) / h);
            std::uint8_t b = static_cast<std::uint8_t>((x + y) % 256);
            if (x > w / 3 && x < 2 * w / 3 && y > h / 3 && y < 2 * h / 3) {
                r = g = b = 240;
            }
            const std::size_t at = (static_cast<std::size_t>(y) * w + x) * 3;
            rgb[at] = r;
            rgb[at + 1] = g;
            rgb[at + 2] = b;
        }
    }
    return rgb;
}

// Runs a SmartBrush refinement through a real encoder + decoder forward pass
// and asserts the alpha plane is a well-formed, frame-sized result in [0, 1].
int exercise_brush(const std::string &encoder, const std::string &decoder)
{
    vision::BrushProvider provider = vision::BrushProvider::load(encoder, decoder);
    check(provider.ok(), "the provider loads: " + provider.error());
    if (!provider.ok()) {
        return 0;
    }

    constexpr std::uint32_t kW = 64;
    constexpr std::uint32_t kH = 48;
    const std::vector<std::uint8_t> rgb = make_frame(kW, kH);
    const vision::FrameView frame{ rgb.data(), kW, kH };

    vision::BrushStroke stroke;
    stroke.frame = 0;
    stroke.tool = Tool::SmartBrush;
    stroke.points = { { 0.5, 0.5 }, { 0.55, 0.5 } };

    std::vector<float> alpha(static_cast<std::size_t>(kW) * kH, 0.0f);
    std::string error;
    const bool ok = provider.refine(alpha, frame, { stroke }, error);

    check(ok, "the forward pass succeeds: " + error);
    check(alpha.size() == static_cast<std::size_t>(kW) * kH, "the alpha plane is width * height");
    bool bounded = true;
    for (const float v : alpha) {
        if (!(v >= 0.0f && v <= 1.0f)) {
            bounded = false;
        }
    }
    check(bounded, "every alpha value stays in 0..1");

    // A non-smart stroke and an empty stroke are skipped, so the same provider
    // reports success with no crash on a no-op refinement.
    vision::BrushStroke plain;
    plain.frame = 0;
    plain.tool = Tool::Brush;
    plain.points = { { 0.5, 0.5 } };
    std::string plain_error;
    check(provider.refine(alpha, frame, { plain }, plain_error),
          "a plain-brush stroke is a clean no-op");

    // A default-constructed provider refuses to run, mirroring the gating path.
    std::vector<float> other(static_cast<std::size_t>(kW) * kH, 0.0f);
    std::string empty_error;
    check(!vision::BrushProvider{ }.refine(other, frame, { stroke }, empty_error),
          "a default provider does not claim success");
    return 0;
}

} // namespace

int main()
{
    const std::string encoder = model_path("GENESIS_BRUSH_ENCODER_MODEL");
    const std::string decoder = model_path("GENESIS_BRUSH_DECODER_MODEL");
    if (encoder.empty() || decoder.empty()) {
        std::printf("SKIP: no brush model set - export GENESIS_BRUSH_ENCODER_MODEL and "
                    "GENESIS_BRUSH_DECODER_MODEL to the .onnx files to run this suite "
                    "(ctest never downloads a model)\n");
        return 0;
    }

    if (std::filesystem::is_regular_file(encoder) && std::filesystem::is_regular_file(decoder)) {
        exercise_brush(encoder, decoder);
    } else {
        std::printf("SKIP: %s or %s is not a readable model\n", encoder.c_str(), decoder.c_str());
    }

    return genesis::test::summary();
}
