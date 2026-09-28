// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <functional>
#include <span>
#include <string>
#include <string_view>

#include "jobs/Job.h"

namespace genesis::ai {

// Tunables for a transcription. An empty language means auto-detect.
struct TranscribeOptions
{
    std::string language;
    int threads = 0; // 0 = whisper.cpp's default thread count
};

// The outcome of a transcription. When ok is false, `error` carries a human
// reason and `transcript` is empty; nothing is half-filled on a failure.
struct TranscribeOutcome
{
    bool ok = false;
    std::string error;
    jobs::TranscriptResult transcript;
};

// Progress: a fraction in 0..1 plus a stage the caller can show ("loading
// model", "transcribing", "done"). Called on the transcribing thread.
using ProgressFn = std::function<void(double fraction, std::string_view stage)>;
// Cancellation: return true to stop. Polled between decoder units; a true
// answer turns the outcome into a graceful ok=false "cancelled".
using AbortFn = std::function<bool()>;

// The whisper.cpp caption provider, engine-free and Qt-free. It turns 16 kHz
// mono float32 PCM into jobs::TranscriptResult segments. The model is loaded
// and freed within each call, so no whisper state outlives a transcribe() and
// nothing leaks on the success, error or abort path.
class WhisperProvider
{
public:
    // Whether whisper.cpp was compiled in. False in the default build
    // (GENESIS_AI_CAPTIONS=OFF), where transcribe() reports "disabled".
    static bool available();

    // "enabled" or "disabled": the runtime state, for logs and UI.
    static std::string_view status();

    // Whether whisper.cpp was built with CUDA GPU support (GGML_CUDA). False
    // when the runtime is disabled or built CPU-only; the app reports the
    // inference mode from this rather than assuming the GPU.
    static bool gpu();

    // Transcribes `pcm` (16 kHz mono float32). `model_path` is the .bin the
    // model store installed. Returns ok=false with a clear reason when the
    // runtime is disabled, the model fails to load, or decoding aborts.
    TranscribeOutcome transcribe(std::string_view model_path, std::span<const float> pcm,
                                 const TranscribeOptions &options = { },
                                 const ProgressFn &progress = { },
                                 const AbortFn &abort = { }) const;

    // Reads a raw little-endian float32 PCM file and transcribes it. The PCM
    // file holds nothing but samples: no header, 4 bytes per sample.
    TranscribeOutcome transcribe_file(std::string_view model_path, std::string_view pcm_path,
                                      const TranscribeOptions &options = { },
                                      const ProgressFn &progress = { },
                                      const AbortFn &abort = { }) const;
};

} // namespace genesis::ai
