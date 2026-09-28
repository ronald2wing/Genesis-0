// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <span>
#include <string_view>

namespace genesis::ai {

// Which execution provider inference runs on. Named as an enum so callers never
// compare raw strings, plus a name function for the stringly values the UI and
// logs need. CPU is always available; the accelerator providers are chosen in a
// platform-appropriate priority order (see preferred_provider) and every one
// falls back to CPU when its append or load fails.
enum class ExecutionProvider {
    Cpu,
    Cuda,
    Tensorrt,
    Rocm,
    Coreml,
    Dml,
    Openvino,
};

// "CPU", "CUDA", "TensorRT", "ROCm", "CoreML", "DirectML" or "OpenVINO": the
// provider's human-facing name.
std::string_view execution_provider_name(ExecutionProvider provider);

// The provider this machine should prefer, given the list of execution-provider
// device names ORT reported (each like "CUDAExecutionProvider" or
// "CPUExecutionProvider"). Priority is TensorRT > CUDA > ROCm > CoreML >
// DirectML > OpenVINO > CPU: on an NVIDIA machine TensorRT wins over CUDA (and
// CUDA over the rest), on AMD ROCm wins, on Apple Silicon CoreML wins, and on
// Windows the GPU-less fallbacks are DirectML then OpenVINO. The platform split
// is implicit - GetEpDevices only reports providers compiled in and present, so
// CoreML cannot appear on Windows and DirectML cannot appear on macOS/Linux. An
// empty or accelerator-free list prefers CPU.
ExecutionProvider preferred_provider(std::span<const std::string_view> device_names);

// Whether make_session() retries on CPU after appending/loading `provider`
// throws. Every accelerator retries on CPU (a present-but-broken runtime, or a
// graph it cannot run); CPU itself has no further fallback, so a failed CPU
// load is a hard error. Pinned so the append-failure -> CPU retry contract is
// testable without an ONNX session.
bool falls_back_to_cpu(ExecutionProvider provider);

// The effective AI execution mode for the status indicator: the canonical name
// of the chosen provider, with " (fallback CPU)" appended when that provider's
// session had to fall back to CPU after a failed load. Computed once and cached
// (thread-safe): in a build without the ONNX runtime it is always "CPU"; with
// it, the mode reflects the execution providers ORT discovered and whether a
// session had to fall back.
std::string_view ai_execution_mode();

} // namespace genesis::ai

#if defined(GENESIS_AI_VISION_ENABLED)

// The ORT-gated half: only available when the ONNX runtime is compiled in, the
// only build that can name an accelerator provider or create a session at all.

#  include <onnxruntime_cxx_api.h>

namespace genesis::ai {

// Creates an ORT session for `model_path`, appending the preferred execution
// provider (see preferred_provider) when one is present and retrying on CPU
// when the append or the load fails (a present-but-broken accelerator runtime,
// or a model that cannot run on it). Records the fallback so ai_execution_mode()
// can report "<provider> (fallback CPU)". Throws Ort::Exception only when even
// the CPU session cannot be created.
Ort::Session make_session(const Ort::Env &env, const char *model_path);

// Appends `provider` to `options`. Throws Ort::Exception on failure, so a
// caller can retry on CPU. CPU appends nothing (it is ORT's implicit default).
void append_provider(Ort::SessionOptions &options, ExecutionProvider provider);

// Records that a non-CPU session had to fall back to CPU. Idempotent and
// thread-safe; read back by ai_execution_mode().
void note_provider_fallback();

} // namespace genesis::ai

#endif // GENESIS_AI_VISION_ENABLED
