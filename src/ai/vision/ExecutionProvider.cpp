// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "ai/vision/ExecutionProvider.h"

#include <span>
#include <string>
#include <string_view>

namespace genesis::ai {

std::string_view execution_provider_name(ExecutionProvider provider)
{
    switch (provider) {
    case ExecutionProvider::Cuda:
        return "CUDA";
    case ExecutionProvider::Cpu:
        return "CPU";
    }
    return "CPU";
}

ExecutionProvider preferred_provider(std::span<const std::string_view> device_names)
{
    for (const std::string_view name : device_names) {
        if (name == "CUDAExecutionProvider") {
            return ExecutionProvider::Cuda;
        }
    }
    return ExecutionProvider::Cpu;
}

} // namespace genesis::ai

#if defined(GENESIS_AI_VISION_ENABLED)

#  include <atomic>
#  include <vector>

#  include <onnxruntime_cxx_api.h>

namespace genesis::ai {

namespace {

// Set once a CUDA session create has failed and been retried on CPU, so the
// status indicator can distinguish "running on the GPU" from "has a GPU but
// fell back". Atomic: written on a worker's session load, read on the UI
// thread's status call.
std::atomic<bool> g_cuda_fell_back{ false };

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
        // A device query that throws is treated as "no CUDA device": the
        // provider stays on CPU rather than failing the session create.
        names.clear();
    }
    return names;
}

bool cuda_available(const Ort::Env &env)
{
    return preferred_provider(device_names(env)) == ExecutionProvider::Cuda;
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

void note_cuda_fallback()
{
    g_cuda_fell_back.store(true, std::memory_order_relaxed);
}

void append_cuda_provider(Ort::SessionOptions &options)
{
    Ort::CUDAProviderOptions cuda_options;
    // AppendExecutionProvider_CUDA_V2 takes the raw OrtCUDAProviderOptionsV2
    // struct by reference, not the CUDAProviderOptions wrapper; the wrapper
    // only converts to a pointer (Base::operator contained_type*), which does
    // not bind to the const-reference parameter. Dereference to pass the
    // underlying struct.
    options.AppendExecutionProvider_CUDA_V2(*cuda_options);
}

Ort::Session make_session(const Ort::Env &env, const char *model_path)
{
    // Prefer CUDA when the env reports a CUDA device. The CUDA append or the
    // load can still fail (a broken runtime, or a graph the GPU cannot run),
    // so a failure falls back to a fresh CPU-only session rather than failing
    // the whole load.
    if (cuda_available(env)) {
        Ort::SessionOptions options = base_options();
        try {
            append_cuda_provider(options);
            return Ort::Session(env, model_path, options);
        } catch (const Ort::Exception &) {
            note_cuda_fallback();
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
    static const std::string probe =
            cuda_available(Ort::Env(ORT_LOGGING_LEVEL_WARNING, "genesis-ai-probe")) ? "CUDA"
                                                                                    : "CPU";
    if (probe == "CUDA" && g_cuda_fell_back.load(std::memory_order_relaxed)) {
        static const std::string fallback = "CUDA (fallback CPU)";
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
