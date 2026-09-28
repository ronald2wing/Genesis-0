// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

// The execution-provider suite: pins the provider naming/preference/fallback
// pure functions and the ai_execution_mode() status contract. Hermetic - no
// model and no ONNX session: preferred_provider() is a pure function over fake
// device-name lists, falls_back_to_cpu() pins the append-failure -> CPU retry,
// and ai_execution_mode() reports "CPU" when the ONNX runtime is not compiled
// in (the default build).

#include <cstdio>
#include <span>
#include <string>
#include <string_view>

#include "ai/vision/ExecutionProvider.h"
#include "ai/vision/SegmentationProvider.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

namespace ai = genesis::ai;
namespace vision = genesis::ai::vision;

void test_execution_provider_name()
{
    check(ai::execution_provider_name(ai::ExecutionProvider::Cpu) == "CPU", "CPU names itself CPU");
    check(ai::execution_provider_name(ai::ExecutionProvider::Cuda) == "CUDA",
          "CUDA names itself CUDA");
    check(ai::execution_provider_name(ai::ExecutionProvider::Tensorrt) == "TensorRT",
          "TensorRT names itself TensorRT");
    check(ai::execution_provider_name(ai::ExecutionProvider::Rocm) == "ROCm",
          "ROCm names itself ROCm");
    check(ai::execution_provider_name(ai::ExecutionProvider::Coreml) == "CoreML",
          "CoreML names itself CoreML");
    check(ai::execution_provider_name(ai::ExecutionProvider::Dml) == "DirectML",
          "DirectML names itself DirectML");
    check(ai::execution_provider_name(ai::ExecutionProvider::Openvino) == "OpenVINO",
          "OpenVINO names itself OpenVINO");
}

void test_preferred_provider()
{
    using ai::ExecutionProvider;

    // An empty device list has no accelerator, so it falls back to CPU.
    const std::span<const std::string_view> none;
    check(ai::preferred_provider(none) == ExecutionProvider::Cpu,
          "an empty device list prefers CPU");

    // A CPU-only list prefers CPU.
    const std::string_view cpu[] = { "CPUExecutionProvider" };
    check(ai::preferred_provider(cpu) == ExecutionProvider::Cpu, "a CPU-only list prefers CPU");

    // Each single-provider list maps to its provider.
    const std::string_view cuda[] = { "CUDAExecutionProvider" };
    check(ai::preferred_provider(cuda) == ExecutionProvider::Cuda, "a CUDA device prefers CUDA");
    const std::string_view trt[] = { "TensorrtExecutionProvider" };
    check(ai::preferred_provider(trt) == ExecutionProvider::Tensorrt,
          "a TensorRT device prefers TensorRT");
    const std::string_view rocm[] = { "ROCMExecutionProvider" };
    check(ai::preferred_provider(rocm) == ExecutionProvider::Rocm, "a ROCm device prefers ROCm");
    const std::string_view coreml[] = { "CoreMLExecutionProvider" };
    check(ai::preferred_provider(coreml) == ExecutionProvider::Coreml,
          "a CoreML device prefers CoreML");
    const std::string_view dml[] = { "DmlExecutionProvider" };
    check(ai::preferred_provider(dml) == ExecutionProvider::Dml,
          "a DirectML device prefers DirectML");
    const std::string_view openvino[] = { "OpenVINOExecutionProvider" };
    check(ai::preferred_provider(openvino) == ExecutionProvider::Openvino,
          "an OpenVINO device prefers OpenVINO");

    // NVIDIA: TensorRT wins over CUDA, and CUDA wins over the rest.
    const std::string_view nvidia_full[] = { "CPUExecutionProvider", "CUDAExecutionProvider",
                                             "TensorrtExecutionProvider", "DmlExecutionProvider" };
    check(ai::preferred_provider(nvidia_full) == ExecutionProvider::Tensorrt,
          "TensorRT is preferred over CUDA when both are present");
    const std::string_view cuda_dml[] = { "CUDAExecutionProvider", "DmlExecutionProvider" };
    check(ai::preferred_provider(cuda_dml) == ExecutionProvider::Cuda,
          "CUDA is preferred over DirectML on Windows without TensorRT");

    // AMD: ROCm wins over DirectML and OpenVINO when all three report.
    const std::string_view amd[] = { "OpenVINOExecutionProvider", "ROCMExecutionProvider",
                                     "DmlExecutionProvider" };
    check(ai::preferred_provider(amd) == ExecutionProvider::Rocm,
          "ROCm is preferred over DirectML and OpenVINO");

    // Windows GPU-less/Intel: DirectML wins over OpenVINO (OpenVINO is last).
    const std::string_view win_intel[] = { "OpenVINOExecutionProvider", "DmlExecutionProvider" };
    check(ai::preferred_provider(win_intel) == ExecutionProvider::Dml,
          "DirectML is preferred over OpenVINO");

    // Apple Silicon: CoreML is the only accelerator reported, and it wins.
    const std::string_view mac[] = { "CPUExecutionProvider", "CoreMLExecutionProvider" };
    check(ai::preferred_provider(mac) == ExecutionProvider::Coreml,
          "CoreML is preferred on a CoreML-only (mac) machine");

    // Order in the reported list does not matter.
    const std::string_view mixed[] = { "CPUExecutionProvider", "CUDAExecutionProvider" };
    check(ai::preferred_provider(mixed) == ExecutionProvider::Cuda,
          "CUDA is preferred regardless of list position");
}

void test_falls_back_to_cpu()
{
    using ai::ExecutionProvider;

    // CPU itself has no further fallback: a failed CPU load is a hard error.
    check(!ai::falls_back_to_cpu(ExecutionProvider::Cpu), "CPU does not fall back to itself");

    // Every accelerator retries on CPU when its append or load throws.
    check(ai::falls_back_to_cpu(ExecutionProvider::Cuda),
          "CUDA retries on CPU after a failed append");
    check(ai::falls_back_to_cpu(ExecutionProvider::Tensorrt),
          "TensorRT retries on CPU after a failed append");
    check(ai::falls_back_to_cpu(ExecutionProvider::Rocm),
          "ROCm retries on CPU after a failed append");
    check(ai::falls_back_to_cpu(ExecutionProvider::Coreml),
          "CoreML retries on CPU after a failed append");
    check(ai::falls_back_to_cpu(ExecutionProvider::Dml),
          "DirectML retries on CPU after a failed append");
    check(ai::falls_back_to_cpu(ExecutionProvider::Openvino),
          "OpenVINO retries on CPU after a failed append");
}

void test_ai_execution_mode()
{
    const std::string_view mode = ai::ai_execution_mode();
    // Without the ONNX runtime there is no execution provider to pick from, so
    // the mode is pinned to CPU.
    if (!vision::SegmentationProvider::available()) {
        check(mode == "CPU", "runtime-free build reports CPU");
        return;
    }
    // With the runtime, the mode is a canonical provider name, optionally with
    // the fallback suffix when a session had to retry on CPU.
    const std::string_view names[] = { "CPU",    "CUDA",     "TensorRT", "ROCm",
                                       "CoreML", "DirectML", "OpenVINO" };
    bool known = false;
    for (const std::string_view name : names) {
        const bool plain = mode == name;
        const bool fell_back = mode.size() > name.size() && mode.substr(0, name.size()) == name
                && mode.substr(name.size()) == " (fallback CPU)";
        known = known || plain || fell_back;
    }
    check(known, "ai_execution_mode reports a known provider (optionally with fallback suffix)");
}

} // namespace

int main()
{
    test_execution_provider_name();
    test_preferred_provider();
    test_falls_back_to_cpu();
    test_ai_execution_mode();

    return genesis::test::summary();
}
