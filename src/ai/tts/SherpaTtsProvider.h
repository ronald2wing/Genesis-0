// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace genesis::ai::tts {

// Which offline-TTS model family the provider loads. Kokoro is the pinned
// multi-speaker model (its voice table is selected by `voice`); PocketTts is
// the pinned zero-shot voice-cloning model (a sample WAV is cloned, no
// transcript needed).
enum class ModelKind {
    Kokoro,
    PocketTts,
};

// Tunables for one synthesis. `voice` is the speaker id from the Kokoro
// model's voice table (0 is the default); `speed` is the speaking rate with
// 1.0 natural. The cloning fields are read only by PocketTts: `reference_wav`
// is the path of the sample WAV whose voice is cloned (PocketTts refuses to run
// without one), `reference_text` is the optional transcript PocketTts ignores
// (kept for a future ZipVoice pin), and `num_steps` the flow-matching step
// count (0 lets the provider use the model default).
struct SynthesizeOptions
{
    std::int32_t voice = 0;
    float speed = 1.0F;
    std::string reference_wav;
    std::string reference_text;
    std::int32_t num_steps = 0;
};

// The outcome of one synthesis. When ok is false, `error` carries a human
// reason and `wav_path` is empty; nothing is half-written on a failure.
struct SynthesizeOutcome
{
    bool ok = false;
    std::string error;
    // Absolute path of the written 16-bit PCM mono WAV.
    std::string wav_path;
    // Seconds of audio, so the caller can say something useful.
    double duration = 0.0;
    // Samples per second of the written WAV, so the importer can describe the
    // media's single audio stream.
    std::uint32_t sample_rate = 0;
};

// The result of unpacking the pinned model tarball into `dir` (a directory
// holding `model.int8.onnx`, `voices.bin`, `tokens.txt` and friends). The
// provider never unpacks: the controller's extractor seam owns that, because
// extraction is a host concern (fork/exec tar) and the provider stays
// engine- and shell-free.
struct ExtractResult
{
    bool ok = false;
    std::string error;
    // The directory the model files were unpacked into.
    std::filesystem::path dir;
};

// Progress: a fraction in 0..1 plus a stage the caller can show ("loading
// model", "synthesizing", "done"). Called on the synthesizing thread.
using ProgressFn = std::function<void(double fraction, std::string_view stage)>;
// Cancellation: return true to stop. Polled before the run; a true answer
// turns the outcome into a graceful ok=false "cancelled".
using AbortFn = std::function<bool()>;

// The sherpa-onnx offline text-to-speech provider, engine-free and Qt-free. It
// turns typed text into a 16-bit PCM mono WAV through the sherpa-onnx offline
// TTS runtime, for either the Kokoro multi-speaker model or the PocketTts
// zero-shot voice-cloning model. The model is loaded and freed within each
// call, so no sherpa-onnx state outlives a synthesize() and nothing leaks on
// the success, error or abort path.
//
// `model_dir` is the directory the model store's tarball was unpacked into:
// for Kokoro it holds `model.int8.onnx`, `voices.bin` and `tokens.txt` (the
// pinned `kokoro-int8-multi-lang-v1_0` package) plus the optional
// `espeak-ng-data` and `dict`; for PocketTts it holds `lm_flow.int8.onnx`,
// `lm_main.int8.onnx`, `encoder.onnx`, `decoder.int8.onnx`,
// `text_conditioner.onnx`, `vocab.json` and `token_scores.json` (the pinned
// `sherpa-onnx-pocket-tts-int8-2026-01-26` package).
class SherpaTtsProvider
{
public:
    // Whether the sherpa-onnx TTS runtime was compiled in. False in the
    // default build (GENESIS_AI_TTS=OFF), where synthesize() reports "disabled".
    static bool available();

    // "enabled" or "disabled": the runtime state, for logs and UI.
    static std::string_view status();

    // Synthesizes `text` to a WAV at `wav_path`. Returns ok=false with a clear
    // reason when the runtime is disabled, the model fails to load, or the run
    // aborts.
    SynthesizeOutcome synthesize(std::string_view model_dir, ModelKind kind, std::string_view text,
                                 const SynthesizeOptions &options = { },
                                 std::string_view wav_path = { }, const ProgressFn &progress = { },
                                 const AbortFn &abort = { }) const;

    // Loads the Kokoro model at `model_dir` and returns the number of speakers
    // in its voice table (SherpaOnnxOfflineTtsNumSpeakers). 0 when the model is
    // absent or fails to load. Expensive - it loads and frees the ONNX graph -
    // so callers run it off the UI thread and cache the result.
    std::int32_t num_speakers(std::string_view model_dir) const;
};

} // namespace genesis::ai::tts
