// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#include "ai/tts/SherpaTtsProvider.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

// The Kokoro model directory to run: GENESIS_KOKORO_MODEL names the unpacked
// directory (holding model.int8.onnx, voices.bin and tokens.txt), else empty.
// ctest never downloads a model; a build without one set skips cleanly.
std::string model_dir()
{
    const char *env = std::getenv("GENESIS_KOKORO_MODEL");
    if (env == nullptr || *env == '\0') {
        return { };
    }
    return env;
}

} // namespace

int main()
{
    const std::string dir = model_dir();
    if (dir.empty()) {
        std::printf("SKIP: no Kokoro model set - export GENESIS_KOKORO_MODEL=/path/to/"
                    "unpacked/kokoro to run this suite (ctest never downloads a "
                    "model)\n");
        return 0;
    }
    if (!std::filesystem::is_directory(dir)) {
        std::printf("SKIP: %s is not a readable model directory\n", dir.c_str());
        return 0;
    }

    genesis::ai::tts::SherpaTtsProvider provider;
    check(provider.available(), "the provider reports available");
    check(provider.status() == "enabled", "the provider reports enabled");

    // The speaker count loads the Kokoro graph off a real unpacked model.
    const std::int32_t speakers = provider.num_speakers(dir);
    check(speakers > 0, "the Kokoro model reports a positive speaker count");

    // One real synthesis: a short utterance into a scratch WAV. The assertions
    // cover only well-formedness (a WAV was written, progress was reported),
    // never the audio content.
    const std::filesystem::path out =
            std::filesystem::temp_directory_path() / "genesis-tts-provider-test.wav";

    bool progressed = false;
    genesis::ai::tts::SynthesizeOptions options;
    options.voice = 0;
    options.speed = 1.0f;
    const auto outcome = provider.synthesize(
            dir, genesis::ai::tts::ModelKind::Kokoro, "Hello, world.", options, out.string(),
            [&](double fraction, std::string_view) {
                progressed = true;
                check(fraction >= 0.0 && fraction <= 1.0, "progress fraction stays in 0..1");
            },
            nullptr);

    check(outcome.ok, "synthesis succeeds: " + outcome.error);
    check(progressed, "progress was reported");
    check(std::filesystem::is_regular_file(out) && std::filesystem::file_size(out) > 0,
          "a WAV was written");
    check(outcome.sample_rate > 0, "the sample rate is positive");
    check(outcome.duration > 0.0, "the duration is positive");
    check(!outcome.wav_path.empty(), "the outcome carries the WAV path");

    std::error_code ec;
    std::filesystem::remove(out, ec);

    return genesis::test::summary();
}
