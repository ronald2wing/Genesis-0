// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "ai/captions/WhisperProvider.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

// The model to run: GENESIS_WHISPER_MODEL names the installed .bin, else empty.
// ctest never downloads a model; a build without one set skips cleanly.
std::string model_path()
{
    const char *env = std::getenv("GENESIS_WHISPER_MODEL");
    if (env == nullptr || *env == '\0') {
        return { };
    }
    return env;
}

// One second of a 440 Hz tone at low amplitude: real samples, so the provider
// sees audio rather than a flat buffer, but content a speech model may return
// nothing for. The test asserts no crash and a well-formed result, never a
// specific transcript.
std::vector<float> synthesize_tone(double seconds, double hz, int sample_rate)
{
    std::vector<float> pcm(static_cast<std::size_t>(seconds * sample_rate));
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        const double t = static_cast<double>(i) / sample_rate;
        pcm[i] = static_cast<float>(0.1 * std::sin(2.0 * 3.141592653589793 * hz * t));
    }
    return pcm;
}

} // namespace

int main()
{
    const std::string model = model_path();
    if (model.empty()) {
        std::printf("SKIP: no whisper model set - export "
                    "GENESIS_WHISPER_MODEL=/path/to/ggml-base.en.bin to run this suite "
                    "(ctest never downloads a model)\n");
        return 0;
    }
    if (!std::filesystem::is_regular_file(model)) {
        std::printf("SKIP: %s is not a readable model file\n", model.c_str());
        return 0;
    }

    genesis::ai::WhisperProvider provider;
    check(provider.available(), "the provider reports available");

    const std::vector<float> pcm = synthesize_tone(1.0, 440.0, 16000);
    const auto outcome =
            provider.transcribe(model, pcm, { }, [&](double fraction, std::string_view) {
                check(fraction >= 0.0 && fraction <= 1.0, "progress fraction stays in 0..1");
            });

    check(outcome.ok, "transcribing the tone succeeds: " + outcome.error);
    // Empty (nothing recognized) or some segments are both acceptable; the
    // contract is only that a segment, when present, is well-formed.
    for (const auto &segment : outcome.transcript.segments) {
        check(segment.start <= segment.end, "a segment starts before it ends");
    }

    return genesis::test::summary();
}
