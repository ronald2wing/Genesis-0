// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "ai/vision/ExecutionProvider.h"

#include <array>
#include <span>
#include <string_view>

namespace genesis::ai {

namespace {

// The accelerator providers in priority order, highest first. preferred_provider()
// walks this and returns the first one ORT reported; CPU is the terminal fallback.
constexpr std::array kAcceleratorPriority = {
    ExecutionProvider::Tensorrt, ExecutionProvider::Cuda, ExecutionProvider::Rocm,
    ExecutionProvider::Coreml,   ExecutionProvider::Dml,  ExecutionProvider::Openvino,
};

// The device name ORT reports each provider under in GetEpDevices (the "type"
// string, e.g. "TensorrtExecutionProvider" - note the lowercase "rt"). A
// platform only ever reports the providers its ORT build ships and the hardware
// actually exposes, which is what makes the fixed priority platform-appropriate.
constexpr std::string_view ep_device_name(ExecutionProvider provider)
{
    switch (provider) {
    case ExecutionProvider::Tensorrt:
        return "TensorrtExecutionProvider";
    case ExecutionProvider::Cuda:
        return "CUDAExecutionProvider";
    case ExecutionProvider::Rocm:
        return "ROCMExecutionProvider";
    case ExecutionProvider::Coreml:
        return "CoreMLExecutionProvider";
    case ExecutionProvider::Dml:
        return "DmlExecutionProvider";
    case ExecutionProvider::Openvino:
        return "OpenVINOExecutionProvider";
    case ExecutionProvider::Cpu:
        return "CPUExecutionProvider";
    }
    return "CPUExecutionProvider";
}

} // namespace

std::string_view execution_provider_name(ExecutionProvider provider)
{
    switch (provider) {
    case ExecutionProvider::Tensorrt:
        return "TensorRT";
    case ExecutionProvider::Cuda:
        return "CUDA";
    case ExecutionProvider::Rocm:
        return "ROCm";
    case ExecutionProvider::Coreml:
        return "CoreML";
    case ExecutionProvider::Dml:
        return "DirectML";
    case ExecutionProvider::Openvino:
        return "OpenVINO";
    case ExecutionProvider::Cpu:
        return "CPU";
    }
    return "CPU";
}

ExecutionProvider preferred_provider(std::span<const std::string_view> device_names)
{
    // The first accelerator in the priority order that ORT reported wins;
    // position in the reported list does not matter. Nothing matching falls
    // through to CPU.
    for (const ExecutionProvider candidate : kAcceleratorPriority) {
        const std::string_view want = ep_device_name(candidate);
        for (const std::string_view name : device_names) {
            if (name == want) {
                return candidate;
            }
        }
    }
    return ExecutionProvider::Cpu;
}

bool falls_back_to_cpu(ExecutionProvider provider)
{
    return provider != ExecutionProvider::Cpu;
}

} // namespace genesis::ai

#if defined(GENESIS_AI_VISION_ENABLED)

#  include <atomic>
#  include <string>
#  include <vector>

#  include <onnxruntime_cxx_api.h>

namespace genesis::ai {

namespace {

// Set once a non-CPU session create has failed and been retried on CPU, so the
// status indicator can distinguish "running on the accelerator" from "has one
// but fell back". Atomic: written on a worker's session load, read on the UI
// thread's status call.
std::atomic<bool> g_fell_back{ false };

// The execution-provider device names ORT reported for `env`, as string views
// into the (lifetime-managed) devices' own storage. Query failure yields an
// empty list, which preferred_provider() reads as CPU.
std::vector<std::string_view> device_names(const Ort::Env &env)
{
    std::vector<std::string_view> names;
    try {
        const std::vector<Ort::ConstEpDevice> devices = env.GetEpDevices();
        names.reserve(devices.size());
        for (const Ort::ConstEpDevice &device : devices) {
            const char *name = device.EpName();
            if (name != nullptr) {
                names.push_back(name);
            }
        }
    } catch (const Ort::Exception &) {
        // A device query that throws is treated as "no accelerator device": the
        // provider stays on CPU rather than failing the session create.
        names.clear();
    }
    return names;
}

// The session options every ORT session shares: let ORT pick the intra-op
// thread count and enable the full graph optimization pass.
Ort::SessionOptions base_options()
{
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(0); // 0 = let ORT pick the thread count
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    return options;
}

} // namespace

void note_provider_fallback()
{
    g_fell_back.store(true, std::memory_order_relaxed);
}

void append_provider(Ort::SessionOptions &options, ExecutionProvider provider)
{
    switch (provider) {
    case ExecutionProvider::Cuda: {
        Ort::CUDAProviderOptions cuda_options;
        // AppendExecutionProvider_CUDA_V2 takes the raw OrtCUDAProviderOptionsV2
        // struct by reference, not the CUDAProviderOptions wrapper; the wrapper
        // only converts to a pointer (Base::operator contained_type*), which
        // does not bind to the const-reference parameter. Dereference to pass
        // the underlying struct.
        options.AppendExecutionProvider_CUDA_V2(*cuda_options);
        return;
    }
    case ExecutionProvider::Tensorrt: {
        Ort::TensorRTProviderOptions trt_options;
        options.AppendExecutionProvider_TensorRT_V2(*trt_options);
        return;
    }
    case ExecutionProvider::Rocm: {
        OrtROCMProviderOptions rocm_options;
        options.AppendExecutionProvider_ROCM(rocm_options);
        return;
    }
    case ExecutionProvider::Openvino:
        options.AppendExecutionProvider_OpenVINO_V2();
        return;
    case ExecutionProvider::Coreml:
        // CoreML has no dedicated append wrapper; the generic append resolves
        // the provider by its registered name.
        options.AppendExecutionProvider("CoreMLExecutionProvider");
        return;
    case ExecutionProvider::Dml:
        // DirectML likewise resolves by name through the generic append. It is
        // only registered on Windows builds that ship the DML provider; on any
        // other host this throws, which make_session() turns into the CPU
        // fallback.
        options.AppendExecutionProvider("DmlExecutionProvider");
        return;
    case ExecutionProvider::Cpu:
        // CPU is ORT's implicit default provider; appending nothing selects it.
        return;
    }
}

Ort::Session make_session(const Ort::Env &env, const char *model_path)
{
    // Prefer the highest-priority accelerator the env reports. The append or
    // the load can still fail (a broken runtime, or a graph the accelerator
    // cannot run), so a failure falls back to a fresh CPU-only session rather
    // than failing the whole load.
    const ExecutionProvider preferred = preferred_provider(device_names(env));
    if (falls_back_to_cpu(preferred)) {
        Ort::SessionOptions options = base_options();
        try {
            append_provider(options, preferred);
            return Ort::Session(env, model_path, options);
        } catch (const Ort::Exception &) {
            note_provider_fallback();
        }
    }
    Ort::SessionOptions options = base_options();
    return Ort::Session(env, model_path, options);
}

std::string_view ai_execution_mode()
{
    // The device probe is the expensive half, so it runs once (magic static);
    // the fallback flag is a cheap atomic read, so a later session fallback is
    // reflected without re-probing.
    static const std::string probe = [] {
        const Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "genesis-ai-probe");
        return std::string(execution_provider_name(preferred_provider(device_names(env))));
    }();
    if (probe != "CPU" && g_fell_back.load(std::memory_order_relaxed)) {
        static const std::string fallback = probe + " (fallback CPU)";
        return fallback;
    }
    return probe;
}

} // namespace genesis::ai

#else // GENESIS_AI_VISION_ENABLED not defined: no ONNX runtime.

namespace genesis::ai {

// Without the ONNX runtime there is no execution provider to pick from, so the
// mode is a constant.
std::string_view ai_execution_mode()
{
    return "CPU";
}

} // namespace genesis::ai

#endif // GENESIS_AI_VISION_ENABLED
