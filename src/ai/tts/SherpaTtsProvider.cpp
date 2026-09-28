// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "ai/tts/SherpaTtsProvider.h"

#if defined(GENESIS_AI_TTS_ENABLED)

#  include <cstring>
#  include <filesystem>
#  include <memory>
#  include <string>

#  include "sherpa-onnx/c-api/c-api.h"

namespace genesis::ai::tts {

namespace {

// RAII ownership of the offline-TTS engine, its generated audio and a decoded
// sample WAV, so all are freed on every exit path (success, error, abort)
// and nothing leaks.
struct TtsDeleter
{
    void operator()(const SherpaOnnxOfflineTts *tts) const { SherpaOnnxDestroyOfflineTts(tts); }
};
using Tts = std::unique_ptr<const SherpaOnnxOfflineTts, TtsDeleter>;

struct AudioDeleter
{
    void operator()(const SherpaOnnxGeneratedAudio *audio) const
    {
        SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
    }
};
using Audio = std::unique_ptr<const SherpaOnnxGeneratedAudio, AudioDeleter>;

struct WaveDeleter
{
    void operator()(const SherpaOnnxWave *wave) const { SherpaOnnxFreeWave(wave); }
};
using Wave = std::unique_ptr<const SherpaOnnxWave, WaveDeleter>;

// The Kokoro model file within `model_dir`: the pinned int8 variant ships
// `model.int8.onnx`; a full-precision variant ships `model.onnx`. Prefer the
// int8 file and fall back so a later pin to a full-precision Kokoro keeps
// working without a provider change.
std::string model_file(std::string_view model_dir)
{
    const std::filesystem::path dir(model_dir);
    const std::filesystem::path int8 = dir / "model.int8.onnx";
    if (std::filesystem::is_regular_file(int8)) {
        return int8.string();
    }
    return (dir / "model.onnx").string();
}

// Builds the offline-TTS engine for `kind` from the unpacked files under
// `model_dir`. Deterministic CPU synthesis: no GPU, so the provider runs in
// the same headless environment the host tests already assume. On failure the
// returned handle is null and `error` carries the reason.
Tts create_tts(ModelKind kind, std::string_view model_dir, std::string &error)
{
    const std::filesystem::path dir(model_dir);

    SherpaOnnxOfflineTtsConfig config;
    std::memset(&config, 0, sizeof(config));
    config.model.num_threads = 1;
    config.model.provider = "cpu";
    config.model.debug = 0;

    if (kind == ModelKind::Kokoro) {
        const std::string model = model_file(model_dir);
        const std::string voices = (dir / "voices.bin").string();
        const std::string tokens = (dir / "tokens.txt").string();
        const std::string data_dir = (dir / "espeak-ng-data").string();
        const std::string dict_dir = (dir / "dict").string();

        config.model.kokoro.model = model.c_str();
        config.model.kokoro.voices = voices.c_str();
        config.model.kokoro.tokens = tokens.c_str();
        // espeak-ng-data and dict are optional: a non-English text normalizes
        // through them, but a missing directory must not refuse the run.
        config.model.kokoro.data_dir =
                std::filesystem::is_directory(data_dir) ? data_dir.c_str() : nullptr;
        config.model.kokoro.dict_dir =
                std::filesystem::is_directory(dict_dir) ? dict_dir.c_str() : nullptr;
        config.model.kokoro.length_scale = 1.0f;
        // The pinned int8 package is a Kokoro >= 1.0 multi-language model:
        // sherpa-onnx refuses to load one unless the caller names a language
        // or a lexicon, and calls _Exit(-1) on refusal - which would kill the
        // whole process, not fail the synthesis. The provider's default voice
        // is English, so name English; a per-call language option would extend
        // this to the other 53 voices.
        config.model.kokoro.lang = "en";

        Tts tts(SherpaOnnxCreateOfflineTts(&config));
        if (!tts) {
            error = "failed to load Kokoro model: " + model;
        }
        return tts;
    }

    // PocketTts: the pinned int8 package ships fixed int8 filenames.
    const std::string lm_flow = (dir / "lm_flow.int8.onnx").string();
    const std::string lm_main = (dir / "lm_main.int8.onnx").string();
    const std::string encoder = (dir / "encoder.onnx").string();
    const std::string decoder = (dir / "decoder.int8.onnx").string();
    const std::string text_conditioner = (dir / "text_conditioner.onnx").string();
    const std::string vocab_json = (dir / "vocab.json").string();
    const std::string token_scores_json = (dir / "token_scores.json").string();

    config.model.pocket.lm_flow = lm_flow.c_str();
    config.model.pocket.lm_main = lm_main.c_str();
    config.model.pocket.encoder = encoder.c_str();
    config.model.pocket.decoder = decoder.c_str();
    config.model.pocket.text_conditioner = text_conditioner.c_str();
    config.model.pocket.vocab_json = vocab_json.c_str();
    config.model.pocket.token_scores_json = token_scores_json.c_str();
    config.model.pocket.voice_embedding_cache_capacity = 50;

    Tts tts(SherpaOnnxCreateOfflineTts(&config));
    if (!tts) {
        error = "failed to load PocketTts model: " + lm_flow;
    }
    return tts;
}

} // namespace

bool SherpaTtsProvider::available()
{
    return true;
}

std::string_view SherpaTtsProvider::status()
{
    return "enabled";
}

std::int32_t SherpaTtsProvider::num_speakers(std::string_view model_dir) const
{
    std::string error;
    Tts tts = create_tts(ModelKind::Kokoro, model_dir, error);
    if (!tts) {
        return 0;
    }
    return SherpaOnnxOfflineTtsNumSpeakers(tts.get());
}

SynthesizeOutcome
SherpaTtsProvider::synthesize(std::string_view model_dir, ModelKind kind, std::string_view text,
                              const SynthesizeOptions &options, std::string_view wav_path,
                              const ProgressFn &progress, const AbortFn &abort) const
{
    SynthesizeOutcome outcome;

    if (text.empty()) {
        outcome.error = "no text to speak";
        return outcome;
    }
    if (wav_path.empty()) {
        outcome.error = "no output path";
        return outcome;
    }
    if (abort && abort()) {
        outcome.error = "cancelled";
        return outcome;
    }

    if (progress) {
        progress(0.0, "loading model");
    }

    std::string load_error;
    Tts tts = create_tts(kind, model_dir, load_error);
    if (!tts) {
        outcome.error = load_error;
        return outcome;
    }

    if (progress) {
        progress(0.3, "synthesizing");
    }

    const float speed = options.speed > 0.0f ? options.speed : 1.0f;
    // The non-deprecated generation entry point: a zeroed GenerationConfig
    // carries the speaker, rate and (for PocketTts) the cloned sample.
    // SherpaOnnxOfflineTtsGenerate is deprecated, so calling it would trip
    // -Werror on a deprecated-declarations warning.
    SherpaOnnxGenerationConfig generation;
    std::memset(&generation, 0, sizeof(generation));
    generation.speed = speed;

    Wave reference;
    if (kind == ModelKind::PocketTts) {
        // PocketTts clones a voice from a sample WAV, so one is required.
        // It is decoded to mono float samples with its own sample rate; the
        // runtime resamples to the model rate as needed. `reference_text` is
        // ignored by PocketTts (zero-shot), but is forwarded so a future
        // ZipVoice pin can reuse the option.
        if (options.reference_wav.empty()) {
            outcome.error = "no reference voice to clone";
            return outcome;
        }
        reference.reset(SherpaOnnxReadWave(std::string(options.reference_wav).c_str()));
        if (!reference) {
            outcome.error = "failed to read the reference voice: " + options.reference_wav;
            return outcome;
        }
        generation.reference_audio = reference->samples;
        generation.reference_audio_len = reference->num_samples;
        generation.reference_sample_rate = reference->sample_rate;
        generation.num_steps = options.num_steps > 0 ? options.num_steps : 5;
        if (!options.reference_text.empty()) {
            // The string must outlive the generate call; the option lives in
            // the caller's options for the whole synthesize().
            generation.reference_text = options.reference_text.c_str();
        }
    } else {
        generation.sid = options.voice;
    }

    Audio audio(SherpaOnnxOfflineTtsGenerateWithConfig(tts.get(), std::string(text).c_str(),
                                                       &generation, nullptr, nullptr));
    if (!audio) {
        outcome.error = "synthesis produced no audio";
        return outcome;
    }

    if (abort && abort()) {
        outcome.error = "cancelled";
        return outcome;
    }

    const int written = SherpaOnnxWriteWave(audio->samples, audio->n, audio->sample_rate,
                                            std::string(wav_path).c_str());
    if (written != 1) {
        outcome.error = "failed to write WAV: " + std::string(wav_path);
        return outcome;
    }

    if (progress) {
        progress(1.0, "done");
    }
    outcome.ok = true;
    outcome.wav_path = std::string(wav_path);
    outcome.sample_rate = static_cast<std::uint32_t>(audio->sample_rate);
    outcome.duration = audio->sample_rate > 0
            ? static_cast<double>(audio->n) / static_cast<double>(audio->sample_rate)
            : 0.0;
    return outcome;
}

} // namespace genesis::ai::tts

#else // GENESIS_AI_TTS_ENABLED not defined: the runtime-free build.

namespace genesis::ai::tts {

bool SherpaTtsProvider::available()
{
    return false;
}

std::string_view SherpaTtsProvider::status()
{
    return "disabled";
}

std::int32_t SherpaTtsProvider::num_speakers(std::string_view) const
{
    return 0;
}

SynthesizeOutcome SherpaTtsProvider::synthesize(std::string_view, ModelKind, std::string_view,
                                                const SynthesizeOptions &, std::string_view,
                                                const ProgressFn &, const AbortFn &) const
{
    SynthesizeOutcome outcome;
    outcome.error = "disabled: sherpa-onnx TTS runtime not compiled in "
                    "(rebuild with GENESIS_AI_TTS=ON)";
    return outcome;
}

} // namespace genesis::ai::tts

#endif // GENESIS_AI_TTS_ENABLED
