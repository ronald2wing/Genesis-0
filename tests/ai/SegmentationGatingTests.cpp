// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <string>
#include <string_view>

#include "ai/vision/SegmentationProvider.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

// The runtime state is reported truthfully: "enabled" or "disabled", and
// available() agrees with status(). This is the gating contract the app leans
// on to decide whether a segmentation can run at all.
void test_runtime_status_reported()
{
    using genesis::ai::vision::SegmentationProvider;
    const std::string_view status = SegmentationProvider::status();
    check(status == "enabled" || status == "disabled", "status is one of enabled/disabled");
    check(SegmentationProvider::available() == (status == "enabled"),
          "available() agrees with status()");
}

// In the runtime-free (default) build load() reports "disabled" and segment()
// yields no alpha, without crashing. Only exercised when the runtime is not
// compiled in - that is the build this test exists to gate.
void test_disabled_reports_cleanly()
{
    using genesis::ai::vision::SegmentationProvider;
    if (SegmentationProvider::available()) {
        return;
    }
    const auto provider = SegmentationProvider::load("/nonexistent/model.onnx",
                                                     genesis::ai::vision::SegmentModel::Rvm);
    check(!provider.ok(), "a disabled provider does not claim it loaded");
    check(provider.error().find("disabled") != std::string::npos,
          "the reason reports the disabled runtime");

    const genesis::ai::vision::FrameView frame{ };
    const auto outcome = provider.segment(frame);
    check(!outcome.ok, "a disabled provider does not claim success");
    check(outcome.error.find("disabled") != std::string::npos,
          "the segment reason reports the disabled runtime");
    check(outcome.alpha.empty(), "a disabled provider yields no alpha");
}

// With the runtime compiled in, a missing model fails gracefully: no crash,
// ok()==false, a non-empty reason. Guards the enabled path's error handling
// without needing a real model on disk.
void test_enabled_missing_model_fails_gracefully()
{
    using genesis::ai::vision::SegmentationProvider;
    if (!SegmentationProvider::available()) {
        return;
    }
    const auto provider = SegmentationProvider::load("/nonexistent/model.onnx",
                                                     genesis::ai::vision::SegmentModel::Rvm);
    check(!provider.ok(), "a missing model does not claim it loaded");
    check(!provider.error().empty(), "the reason is non-empty");

    const genesis::ai::vision::FrameView frame{ };
    const auto outcome = provider.segment(frame);
    check(!outcome.ok, "a failed load does not claim success");
    check(!outcome.error.empty(), "the segment reason is non-empty");
}

} // namespace

int main()
{
    test_runtime_status_reported();
    test_disabled_reports_cleanly();
    test_enabled_missing_model_fails_gracefully();

    return genesis::test::summary();
}
