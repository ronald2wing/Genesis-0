// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "ai/captions/WhisperProvider.h"

#if defined(GENESIS_AI_CAPTIONS_ENABLED)

#  include <fstream>
#  include <memory>
#  include <string>
#  include <utility>
#  include <vector>

#  include "whisper.h"

namespace genesis::ai {

namespace {

// RAII ownership of the whisper context, so whisper_free runs on every exit
// path (success, error, abort) and the context never leaks.
struct WhisperContextDeleter
{
    void operator()(whisper_context *ctx) const { whisper_free(ctx); }
};
using Context = std::unique_ptr<whisper_context, WhisperContextDeleter>;

// The state both whisper C callbacks read through their void* user_data. One
// struct, so the two hooks share a single pointer and no const_cast is needed.
struct CallbackState
{
    const ProgressFn *progress = nullptr;
    const AbortFn *abort = nullptr;
    bool cancelled = false;
};

// Whisper reports progress 0..100; translate to a fraction for the caller.
void progress_cb(whisper_context *, whisper_state *, int progress, void *user_data)
{
    const auto *state = static_cast<const CallbackState *>(user_data);
    if (state->progress != nullptr && *state->progress) {
        (*state->progress)(static_cast<double>(progress) / 100.0, "transcribing");
    }
}

// Whisper polls the abort hook between decoder units; a true answer stops the
// decode. The flag records that the stop came from the caller, so the outcome
// can say "cancelled" rather than "decode failed".
bool abort_cb(void *user_data)
{
    auto *state = static_cast<CallbackState *>(user_data);
    if (state->abort != nullptr && *state->abort && (*state->abort)()) {
        state->cancelled = true;
        return true;
    }
    return false;
}

// Reads a raw little-endian float32 PCM file (no header, 4 bytes/sample).
bool read_f32_file(std::string_view path, std::vector<float> &out)
{
    std::ifstream in(std::string(path), std::ios::binary);
    if (!in) {
        return false;
    }
    out.clear();
    float sample = 0.0f;
    while (in.read(reinterpret_cast<char *>(&sample), sizeof(sample))) {
        out.push_back(sample);
    }
    return !out.empty();
}

} // namespace

bool WhisperProvider::available()
{
    return true;
}

std::string_view WhisperProvider::status()
{
    return "enabled";
}

bool WhisperProvider::gpu()
{
#  if defined(GENESIS_AI_GPU_ENABLED)
    return true;
#  else
    return false;
#  endif
}

TranscribeOutcome WhisperProvider::transcribe(std::string_view model_path,
                                              std::span<const float> pcm,
                                              const TranscribeOptions &options,
                                              const ProgressFn &progress,
                                              const AbortFn &abort) const
{
    TranscribeOutcome outcome;

    if (pcm.empty()) {
        outcome.error = "no PCM input";
        return outcome;
    }

    if (progress) {
        progress(0.0, "loading model");
    }

    // Prefer the GPU when whisper.cpp was built with CUDA support; fall back to
    // a CPU decode otherwise. The device is the first CUDA device (0), matching
    // whisper.cpp's own default.
    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = WhisperProvider::gpu();
    if (cparams.use_gpu) {
        cparams.gpu_device = 0;
    }

    Context ctx(whisper_init_from_file_with_params(std::string(model_path).c_str(), cparams));
    if (!ctx) {
        outcome.error = "failed to load model: " + std::string(model_path);
        return outcome;
    }

    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.print_progress = false;
    wparams.print_realtime = false;
    wparams.print_timestamps = false;
    wparams.print_special = false;
    wparams.translate = false;
    wparams.no_context = true;
    wparams.language = options.language.empty() ? "auto" : options.language.c_str();
    if (options.threads > 0) {
        wparams.n_threads = options.threads;
    }

    CallbackState callbacks{ &progress, &abort, false };
    wparams.progress_callback = progress_cb;
    wparams.progress_callback_user_data = &callbacks;
    wparams.abort_callback = abort_cb;
    wparams.abort_callback_user_data = &callbacks;

    const int rc = whisper_full(ctx.get(), wparams, pcm.data(), static_cast<int>(pcm.size()));

    if (callbacks.cancelled) {
        outcome.error = "cancelled";
        return outcome;
    }
    if (rc != 0) {
        outcome.error = "decode failed (code " + std::to_string(rc) + ")";
        return outcome;
    }

    const int n_segments = whisper_full_n_segments(ctx.get());
    outcome.transcript.segments.reserve(static_cast<std::size_t>(n_segments));
    for (int i = 0; i < n_segments; ++i) {
        const char *text = whisper_full_get_segment_text(ctx.get(), i);
        if (text == nullptr) {
            continue;
        }
        jobs::CaptionSegment segment;
        segment.start = static_cast<double>(whisper_full_get_segment_t0(ctx.get(), i)) / 100.0;
        segment.end = static_cast<double>(whisper_full_get_segment_t1(ctx.get(), i)) / 100.0;
        segment.text = text;
        outcome.transcript.segments.push_back(std::move(segment));
    }

    if (progress) {
        progress(1.0, "done");
    }
    outcome.ok = true;
    return outcome;
}

TranscribeOutcome WhisperProvider::transcribe_file(std::string_view model_path,
                                                   std::string_view pcm_path,
                                                   const TranscribeOptions &options,
                                                   const ProgressFn &progress,
                                                   const AbortFn &abort) const
{
    std::vector<float> pcm;
    if (!read_f32_file(pcm_path, pcm)) {
        TranscribeOutcome outcome;
        outcome.error = "failed to read PCM file: " + std::string(pcm_path);
        return outcome;
    }
    return transcribe(model_path, pcm, options, progress, abort);
}

} // namespace genesis::ai

#else // GENESIS_AI_CAPTIONS_ENABLED not defined: the runtime-free build.

namespace genesis::ai {

bool WhisperProvider::available()
{
    return false;
}

std::string_view WhisperProvider::status()
{
    return "disabled";
}

bool WhisperProvider::gpu()
{
    return false;
}

TranscribeOutcome WhisperProvider::transcribe(std::string_view, std::span<const float>,
                                              const TranscribeOptions &, const ProgressFn &,
                                              const AbortFn &) const
{
    TranscribeOutcome outcome;
    outcome.error = "disabled: whisper.cpp runtime not compiled in "
                    "(rebuild with GENESIS_AI_CAPTIONS=ON)";
    return outcome;
}

TranscribeOutcome WhisperProvider::transcribe_file(std::string_view, std::string_view,
                                                   const TranscribeOptions &, const ProgressFn &,
                                                   const AbortFn &) const
{
    TranscribeOutcome outcome;
    outcome.error = "disabled: whisper.cpp runtime not compiled in "
                    "(rebuild with GENESIS_AI_CAPTIONS=ON)";
    return outcome;
}

} // namespace genesis::ai

#endif // GENESIS_AI_CAPTIONS_ENABLED
