// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

// The execution-provider suite: pins the pure provider naming/preference
// helpers and the ai_execution_mode() status contract. Hermetic - no model and
// no ONNX session: preferred_provider() is a pure function over fake
// device-name lists, and ai_execution_mode() reports "CPU" when the ONNX
// runtime is not compiled in (the default build).

#include <cstdio>
#include <span>
#include <string>
#include <string_view>

#include "ai/vision/ExecutionProvider.h"
#include "ai/vision/SegmentationProvider.h"

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const std::string &what)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

namespace ai = genesis::ai;
namespace vision = genesis::ai::vision;

void test_execution_provider_name()
{
    check(ai::execution_provider_name(ai::ExecutionProvider::Cpu) == "CPU", "CPU names itself CPU");
    check(ai::execution_provider_name(ai::ExecutionProvider::Cuda) == "CUDA",
          "CUDA names itself CUDA");
}

void test_preferred_provider()
{
    using ai::ExecutionProvider;

    // An empty device list has no CUDA device, so it falls back to CPU.
    const std::span<const std::string_view> none;
    check(ai::preferred_provider(none) == ExecutionProvider::Cpu,
          "an empty device list prefers CPU");

    // A CPU-only list prefers CPU.
    const std::string_view cpu[] = { "CPUExecutionProvider" };
    check(ai::preferred_provider(cpu) == ExecutionProvider::Cpu, "a CPU-only list prefers CPU");

    // A CUDA device present means CUDA is preferred.
    const std::string_view cuda[] = { "CUDAExecutionProvider" };
    check(ai::preferred_provider(cuda) == ExecutionProvider::Cuda, "a CUDA device prefers CUDA");

    // Non-CUDA GPU providers (TensorRT, ROCm, OpenVINO) are not CUDA: they must
    // not flip the preference off CPU.
    const std::string_view other_gpus[] = { "TensorrtExecutionProvider", "ROCMExecutionProvider",
                                            "OpenVINOExecutionProvider" };
    check(ai::preferred_provider(other_gpus) == ExecutionProvider::Cpu,
          "non-CUDA GPU providers still prefer CPU");

    // CUDA is preferred exactly when a CUDA device is present, wherever it
    // appears in the list.
    const std::string_view mixed[] = { "CPUExecutionProvider", "CUDAExecutionProvider" };
    check(ai::preferred_provider(mixed) == ExecutionProvider::Cuda,
          "CUDA is preferred regardless of list position");
}

void test_ai_execution_mode()
{
    const std::string_view mode = ai::ai_execution_mode();
    check(mode == "CPU" || mode == "CUDA" || mode == "CUDA (fallback CPU)",
          "ai_execution_mode reports a known value");
    // Without the ONNX runtime there is no execution provider to pick from, so
    // the mode is pinned to CPU.
    if (!vision::SegmentationProvider::available()) {
        check(mode == "CPU", "runtime-free build reports CPU");
    }
}

} // namespace

int main()
{
    test_execution_provider_name();
    test_preferred_provider();
    test_ai_execution_mode();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
