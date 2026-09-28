// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <span>
#include <string_view>

namespace genesis::ai {

// Which execution provider inference runs on. Named as an enum so callers never
// compare raw strings, plus a name function for the stringly values the UI and
// logs need. Only CPU and CUDA are wired; the other ORT providers (TensorRT,
// ROCm, OpenVINO) are out of scope for the host/engine split today.
enum class ExecutionProvider {
    Cpu,
    Cuda,
};

// "CPU" or "CUDA": the provider's human-facing name.
std::string_view execution_provider_name(ExecutionProvider provider);

// The provider this machine should prefer, given the list of execution-provider
// device names ORT reported (each like "CUDAExecutionProvider" or
// "CPUExecutionProvider"). CUDA is preferred exactly when a CUDA device is
// present; anything else falls back to CPU.
ExecutionProvider preferred_provider(std::span<const std::string_view> device_names);

// The effective AI execution mode for the status indicator: "CPU", "CUDA", or
// "CUDA (fallback CPU)". Computed once and cached (thread-safe): in a build
// without the ONNX runtime it is always "CPU"; with it, the mode reflects the
// execution providers ORT discovered and whether a CUDA session had to fall
// back to CPU after a failed load.
std::string_view ai_execution_mode();

} // namespace genesis::ai

#if defined(GENESIS_AI_VISION_ENABLED)

// The ORT-gated half: only available when the ONNX runtime is compiled in, the
// only build that can name a CUDA provider or create a session at all.

#  include <onnxruntime_cxx_api.h>

namespace genesis::ai {

// Creates an ORT session for `model_path`, appending the CUDA execution
// provider when one is present and retrying on CPU when the CUDA append or the
// load fails (a present-but-broken CUDA runtime, or a model that cannot run on
// the GPU). Records a CUDA-to-CPU fallback so ai_execution_mode() can report
// "CUDA (fallback CPU)". Throws Ort::Exception only when even the CPU session
// cannot be created.
Ort::Session make_session(const Ort::Env &env, const char *model_path);

// Appends the CUDA execution provider to `options`. Throws Ort::Exception on
// failure, so a caller can retry on CPU.
void append_cuda_provider(Ort::SessionOptions &options);

// Records that a CUDA session had to fall back to CPU. Idempotent and
// thread-safe; read back by ai_execution_mode().
void note_cuda_fallback();

} // namespace genesis::ai

#endif // GENESIS_AI_VISION_ENABLED
