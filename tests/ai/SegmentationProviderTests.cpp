// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "ai/vision/SegmentationProvider.h"

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const std::string &what)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

// The model to run: GENESIS_RVM_MODEL / GENESIS_ISNET_MODEL name the installed
// .onnx files, else empty. ctest never downloads a model; a build without one
// set skips that model cleanly.
std::string model_path(const char *env)
{
    const char *value = std::getenv(env);
    if (value == nullptr || *value == '\0') {
        return { };
    }
    return value;
}

// A synthetic RGB frame: a vertical gradient plus a bright centre block, so the
// model sees real content rather than a flat buffer. The assertions only cover
// well-formedness - never a specific mask - because the models are content.
std::vector<std::uint8_t> make_frame(std::uint32_t w, std::uint32_t h)
{
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(w) * h * 3);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t r = static_cast<std::uint8_t>((x * 255) / w);
            std::uint8_t g = static_cast<std::uint8_t>((y * 255) / h);
            std::uint8_t b = static_cast<std::uint8_t>((x + y) % 256);
            // A bright square in the middle stands out against the gradient.
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

// Runs one model through a forward pass and asserts the outcome is a
// well-formed alpha plane at the frame's own size, in [0, 1]. Returns the
// number of failures this run added.
int exercise_model(const std::string &model, genesis::ai::vision::SegmentModel kind)
{
    genesis::ai::vision::SegmentationProvider provider =
            genesis::ai::vision::SegmentationProvider::load(model, kind);
    check(provider.ok(),
          "the provider loads " + std::string(genesis::ai::vision::segment_model_name(kind)) + ": "
                  + provider.error());
    if (!provider.ok()) {
        return 0;
    }

    constexpr std::uint32_t kW = 48;
    constexpr std::uint32_t kH = 32;
    const std::vector<std::uint8_t> rgb = make_frame(kW, kH);
    const genesis::ai::vision::FrameView frame{ rgb.data(), kW, kH };

    bool progressed = false;
    const auto outcome = provider.segment(
            frame,
            [&](double fraction, std::string_view) {
                progressed = true;
                check(fraction >= 0.0 && fraction <= 1.0, "progress fraction stays in 0..1");
            },
            nullptr);

    check(outcome.ok, "the forward pass succeeds: " + outcome.error);
    check(progressed, "progress was reported");
    check(outcome.width == kW && outcome.height == kH, "the mask is the frame's own size");
    check(outcome.alpha.size() == static_cast<std::size_t>(kW) * kH,
          "the alpha plane is width * height");
    bool bounded = true;
    for (const float v : outcome.alpha) {
        if (!(v >= 0.0f && v <= 1.0f)) {
            bounded = false;
        }
    }
    check(bounded, "every alpha value stays in 0..1");

    // A default-constructed provider refuses to run, mirroring the gating path.
    const auto empty = genesis::ai::vision::SegmentationProvider{ }.segment(frame);
    check(!empty.ok, "a default provider does not claim success");
    return 0;
}

} // namespace

int main()
{
    const std::string rvm = model_path("GENESIS_RVM_MODEL");
    const std::string isnet = model_path("GENESIS_ISNET_MODEL");
    if (rvm.empty() && isnet.empty()) {
        std::printf("SKIP: no segmentation model set - export GENESIS_RVM_MODEL or "
                    "GENESIS_ISNET_MODEL to a .onnx file to run this suite (ctest "
                    "never downloads a model)\n");
        return 0;
    }

    if (!rvm.empty() && std::filesystem::is_regular_file(rvm)) {
        exercise_model(rvm, genesis::ai::vision::SegmentModel::Rvm);
    } else if (!rvm.empty()) {
        std::printf("SKIP: %s is not a readable RVM model\n", rvm.c_str());
    }

    if (!isnet.empty() && std::filesystem::is_regular_file(isnet)) {
        exercise_model(isnet, genesis::ai::vision::SegmentModel::IsNet);
    } else if (!isnet.empty()) {
        std::printf("SKIP: %s is not a readable IS-Net model\n", isnet.c_str());
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
