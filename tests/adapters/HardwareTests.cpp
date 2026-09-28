// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

// The hardware capability probe's suite: the pure renderer-string
// classification (software vs hardware) with fake strings, the element validity
// gate (a present-but-broken factory must not answer), and the probe's
// self-consistency. Headless and GPU-agnostic: the GL renderer readback is
// best-effort, so its result is checked for consistency rather than a specific
// value.

#include <cstdio>
#include <string>

#include <ges/ges.h>
#include <gst/gst.h>

#include "adapters/engine/ges/export/Hardware.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

namespace ges = genesis::adapters::engine::ges;

void test_software_renderer_detection()
{
    check(ges::is_software_renderer("llvmpipe (LLVM 16.0.6, 256 bits)"),
          "llvmpipe is a software renderer");
    check(ges::is_software_renderer("softpipe"), "softpipe is a software renderer");
    check(ges::is_software_renderer("Gallium 0.4 on llvmpipe"),
          "a Gallium llvmpipe renderer is software");
    check(ges::is_software_renderer("Mesa DRI swrast"), "swrast is a software renderer");
    check(ges::is_software_renderer("Apple Software Renderer"),
          "a generic software rasteriser is software");
    check(!ges::is_software_renderer("NVIDIA GeForce RTX 3060/PCIe/SSE2"),
          "an NVIDIA GPU is not software");
    check(!ges::is_software_renderer("Mesa Intel(R) HD Graphics 620 (KBL GT2)"),
          "an Intel iGPU is not software");
    check(!ges::is_software_renderer("AMD Radeon RX 6700 XT"), "an AMD GPU is not software");
    check(!ges::is_software_renderer(""), "an empty renderer is not software");
    // The rasteriser names must match regardless of case.
    check(ges::is_software_renderer("LLVMPIPE (LLVM 16)"),
          "llvmpipe detection is case-insensitive");
}

void test_nvidia_classification()
{
    check(ges::is_nvidia_renderer("NVIDIA GeForce RTX 3060/PCIe/SSE2"),
          "an NVIDIA GPU renderer is NVIDIA");
    check(!ges::is_nvidia_renderer("Mesa Intel(R) UHD Graphics 630 (CFL GT2)"),
          "an Intel iGPU renderer is not NVIDIA");
    check(!ges::is_nvidia_renderer("llvmpipe (LLVM 16.0.6, 256 bits)"),
          "a software renderer is not NVIDIA");
    check(!ges::is_nvidia_renderer(""), "an empty renderer is not NVIDIA");
    check(ges::is_nvidia_renderer("nvidia tesla t4"), "NVIDIA detection is case-insensitive");

    // The mismatch is only reported when NVIDIA is present but the GL context
    // did not land on it.
    check(ges::nvidia_mismatch("Mesa Intel(R) UHD Graphics 630", /*nvidia_available=*/true),
          "a non-NVIDIA renderer with NVIDIA present is a mismatch");
    check(!ges::nvidia_mismatch("NVIDIA GeForce RTX 3060", /*nvidia_available=*/true),
          "an NVIDIA renderer with NVIDIA present is not a mismatch");
    check(!ges::nvidia_mismatch("Mesa Intel(R) UHD Graphics 630", /*nvidia_available=*/false),
          "a non-NVIDIA renderer without NVIDIA is not a mismatch");

    // The hint is emitted exactly when there is a mismatch, and names both the
    // renderer and the offload environment.
    const std::string hint = ges::nvidia_offload_hint("Mesa Intel(R) UHD Graphics 630",
                                                      /*nvidia_available=*/true);
    check(!hint.empty(), "a mismatch produces a hint");
    check(hint.find("__NV_PRIME_RENDER_OFFLOAD=1") != std::string::npos,
          "the hint names the PRIME offload variable");
    check(hint.find("__GLX_VENDOR_LIBRARY_NAME=nvidia") != std::string::npos,
          "the hint names the NVIDIA GLX library variable");
    check(hint.find("Mesa Intel(R) UHD Graphics 630") != std::string::npos,
          "the hint names the active renderer");
    check(ges::nvidia_offload_hint("NVIDIA GeForce RTX 3060", /*nvidia_available=*/true).empty(),
          "no hint when the renderer already is NVIDIA");
    check(ges::nvidia_offload_hint("Mesa Intel(R) UHD Graphics 630", /*nvidia_available=*/false)
                  .empty(),
          "no hint when NVIDIA is absent");
}

void test_element_usable()
{
    // Universal elements are present and READY-capable everywhere.
    check(ges::element_usable("fakesink"), "fakesink is present and usable");
    check(ges::element_usable("videotestsrc"), "videotestsrc is present and usable");
    // A name no registry has must refuse.
    check(!ges::element_usable("genesis-element-that-does-not-exist"),
          "an unknown element refuses");
}

void test_probe_self_consistency()
{
    const ges::HardwareInfo info = ges::probe_hardware();
    // The software flag must agree with the renderer string's own
    // classification, whatever this machine returned.
    check(info.software_gl == ges::is_software_renderer(info.renderer),
          "software_gl agrees with the renderer string");
    // The encode element is named exactly when hardware encode is reported.
    check(info.hardware_encode == !info.encode_element.empty(),
          "hardware_encode implies a named element, and conversely");
    // nvidia_available is reported whenever any NVIDIA element was found (the
    // device-node signal can only add more, never contradict the elements).
    check(info.nvidia_elements.empty() || info.nvidia_available,
          "non-empty nvidia elements imply nvidia_available");
    // Report what the probe saw, so a headless observer can read the outcome.
    std::string nvidia_joined;
    for (const std::string &element : info.nvidia_elements) {
        if (!nvidia_joined.empty()) {
            nvidia_joined += ',';
        }
        nvidia_joined += element;
    }
    std::printf("probe: renderer=%s software_gl=%d hardware_decode=%d "
                "hardware_encode=%d encode_element=%s nvidia_available=%d "
                "nvidia_elements=%s\n",
                info.renderer.c_str(), info.software_gl ? 1 : 0, info.hardware_decode ? 1 : 0,
                info.hardware_encode ? 1 : 0, info.encode_element.c_str(),
                info.nvidia_available ? 1 : 0, nvidia_joined.c_str());
}

} // namespace

int main()
{
    gst_init(nullptr, nullptr);
    ges_init();

    test_software_renderer_detection();
    test_nvidia_classification();
    test_element_usable();
    test_probe_self_consistency();

    return genesis::test::summary();
}
