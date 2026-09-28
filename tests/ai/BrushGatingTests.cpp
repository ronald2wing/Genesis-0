// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "ai/vision/BrushProvider.h"

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

namespace vision = genesis::ai::vision;
using Tool = genesis::project::BrushTool;

// The runtime state is reported truthfully: "enabled" or "disabled", and
// available() agrees with status(). This is the gating contract the app leans
// on to decide whether a smart-brush refinement can run at all.
void test_runtime_status_reported()
{
    const std::string_view status = vision::BrushProvider::status();
    check(status == "enabled" || status == "disabled", "status is one of enabled/disabled");
    check(vision::BrushProvider::available() == (status == "enabled"),
          "available() agrees with status()");
}

// In the runtime-free (default) build load() reports "disabled" and refine()
// leaves the alpha untouched, without crashing. Only exercised when the runtime
// is not compiled in - that is the build this test exists to gate.
void test_disabled_reports_cleanly()
{
    if (vision::BrushProvider::available()) {
        return;
    }
    auto provider = vision::BrushProvider::load("/nonexistent/e.onnx", "/nonexistent/d.onnx");
    check(!provider.ok(), "a disabled provider does not claim it loaded");
    check(provider.error().find("disabled") != std::string::npos,
          "the reason reports the disabled runtime");

    std::vector<float> alpha{ 0.5f, 0.5f, 0.5f, 0.5f };
    std::string error;
    const bool ok = provider.refine(alpha, vision::FrameView{ }, { }, error);
    check(!ok, "a disabled provider does not claim success");
    check(error.find("disabled") != std::string::npos,
          "the refine reason reports the disabled runtime");
    check(alpha == std::vector<float>({ 0.5f, 0.5f, 0.5f, 0.5f }),
          "a disabled refine leaves the alpha untouched");
}

// With the runtime compiled in, a missing model fails gracefully: no crash,
// ok()==false, a non-empty reason. Guards the enabled path's error handling
// without needing a real model on disk.
void test_enabled_missing_model_fails_gracefully()
{
    if (!vision::BrushProvider::available()) {
        return;
    }
    auto provider = vision::BrushProvider::load("/nonexistent/e.onnx", "/nonexistent/d.onnx");
    check(!provider.ok(), "a missing model does not claim it loaded");
    check(!provider.error().empty(), "the reason is non-empty");

    std::vector<float> alpha{ 0.5f, 0.5f, 0.5f, 0.5f };
    std::string error;
    const bool ok = provider.refine(alpha, vision::FrameView{ }, { }, error);
    check(!ok, "a failed load does not claim success");
    check(!error.empty(), "the refine reason is non-empty");
}

// brush_input_size maps a frame onto the encoder's 1024-long-side input: the
// long side becomes 1024, the short side scales to preserve aspect, and a
// zero-sized frame maps to a zero resized size. Pure arithmetic, so it runs in
// every build.
void test_brush_input_size()
{
    using vision::brush_input_size;
    using vision::brush_model_size;

    const vision::BrushInputSize square = brush_input_size(1024, 1024);
    check(square.scale == 1.0, "a square frame scales by 1.0");
    check(square.resized_width == 1024 && square.resized_height == 1024,
          "a square frame maps to 1024x1024");

    const vision::BrushInputSize landscape = brush_input_size(2048, 1024);
    check(std::fabs(landscape.scale - 0.5) < 1e-9, "a 2:1 frame scales by 0.5");
    check(landscape.resized_width == brush_model_size && landscape.resized_height == 512,
          "the long side is 1024 and the short side 512");

    const vision::BrushInputSize portrait = brush_input_size(1024, 2048);
    check(portrait.resized_width == 512 && portrait.resized_height == brush_model_size,
          "a portrait frame keeps the long side 1024");

    const vision::BrushInputSize empty = brush_input_size(0, 0);
    check(empty.resized_width == 0 && empty.resized_height == 0,
          "a zero-sized frame maps to a zero resized size");
}

// brush_combine blends one stroke's refined mask per its tool: SmartBrush
// keeps (union), SmartEraser drops (subtract), and a plain brush/eraser or a
// shape mismatch is a no-op. Pure arithmetic, so it runs in every build.
void test_brush_combine()
{
    using vision::brush_combine;

    std::vector<float> alpha{ 0.5f };
    std::vector<float> refined{ 0.8f };
    brush_combine(alpha, refined, Tool::SmartBrush);
    check(alpha == std::vector<float>({ 0.8f }), "SmartBrush takes the union (max)");

    alpha = { 0.9f };
    refined = { 0.3f };
    brush_combine(alpha, refined, Tool::SmartBrush);
    check(alpha == std::vector<float>({ 0.9f }), "SmartBrush never lowers an already-kept pixel");

    alpha = { 0.8f };
    refined = { 0.5f };
    brush_combine(alpha, refined, Tool::SmartEraser);
    check(std::fabs(alpha[0] - 0.4f) < 1e-6f, "SmartEraser subtracts (alpha * (1 - refined))");

    alpha = { 0.8f };
    brush_combine(alpha, refined, Tool::Brush);
    check(alpha == std::vector<float>({ 0.8f }), "a plain Brush paints a disc, not a model result");

    alpha = { 0.8f };
    brush_combine(alpha, refined, Tool::Eraser);
    check(alpha == std::vector<float>({ 0.8f }),
          "a plain Eraser paints a disc, not a model result");

    alpha = { 0.8f, 0.8f };
    brush_combine(alpha, refined, Tool::SmartBrush);
    check(alpha == std::vector<float>({ 0.8f, 0.8f }),
          "a shape mismatch is a no-op, never a crash");
}

} // namespace

int main()
{
    test_runtime_status_reported();
    test_disabled_reports_cleanly();
    test_enabled_missing_model_fails_gracefully();
    test_brush_input_size();
    test_brush_combine();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
