// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "ai/captions/WhisperProvider.h"

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

// The runtime state is reported truthfully: "enabled" or "disabled", and
// available() agrees with status(). This is the gating contract the app leans
// on to decide whether a transcription can run at all.
void test_runtime_status_reported()
{
    using genesis::ai::WhisperProvider;
    const std::string_view status = WhisperProvider::status();
    check(status == "enabled" || status == "disabled", "status is one of enabled/disabled");
    check(WhisperProvider::available() == (status == "enabled"),
          "available() agrees with status()");
}

// In the runtime-free (default) build a transcribe() reports "disabled" and
// yields no segments, without crashing. Only exercised when the runtime is not
// compiled in - that is the build this test exists to gate.
void test_disabled_reports_cleanly()
{
    using genesis::ai::WhisperProvider;
    if (WhisperProvider::available()) {
        return;
    }
    const std::vector<float> pcm(16000, 0.0f);
    const auto outcome = WhisperProvider{ }.transcribe("/nonexistent/model.bin", pcm);
    check(!outcome.ok, "a disabled provider does not claim success");
    check(outcome.error.find("disabled") != std::string::npos,
          "the reason reports the disabled runtime");
    check(outcome.transcript.segments.empty(), "a disabled provider yields no segments");
}

// With the runtime compiled in, a missing model fails gracefully: no crash,
// ok=false, a non-empty reason, no segments. Guards the enabled path's error
// handling without needing a real model on disk.
void test_enabled_missing_model_fails_gracefully()
{
    using genesis::ai::WhisperProvider;
    if (!WhisperProvider::available()) {
        return;
    }
    const std::vector<float> pcm(16000, 0.0f);
    const auto outcome = WhisperProvider{ }.transcribe("/nonexistent/model.bin", pcm);
    check(!outcome.ok, "a missing model does not claim success");
    check(!outcome.error.empty(), "the reason is non-empty");
    check(outcome.transcript.segments.empty(), "no segments on a model-load failure");
}

} // namespace

int main()
{
    test_runtime_status_reported();
    test_disabled_reports_cleanly();
    test_enabled_missing_model_fails_gracefully();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
