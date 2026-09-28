// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace genesis::adapters::engine::ges {

// What the hardware probe found about this machine's video acceleration. A
// plain struct so the engine-neutral app controller and the headless tests can
// consume it without touching GStreamer.
struct HardwareInfo
{
    // The GL renderer string the probe read back from a freshly created GL
    // context (e.g. "NVIDIA GeForce RTX 3060/PCIe/SSE2" or "llvmpipe (LLVM
    // 16.0.6, 256 bits)"). Empty when no GL context could be made.
    std::string renderer;
    // Whether that renderer is a software rasteriser (llvmpipe/softpipe/swrast
    // or any string that names itself software).
    bool software_gl = false;
    // Whether a hardware video decoder exists (a decoder factory classified
    // "Hardware" that the distro has not disabled).
    bool hardware_decode = false;
    // Whether a hardware video encoder answered the runtime validity probe.
    bool hardware_encode = false;
    // The encoder element that answered, or empty when encoding in software.
    std::string encode_element;
    // Whether an NVIDIA GPU/driver stack is present, independent of the GL
    // renderer: at least one NVIDIA element factory (nvenc/nvdec/nvcuda/
    // nvv4l2) is installed, or an /dev/nvidia* device node exists. A hybrid/
    // PRIME machine whose GL context landed on the Intel iGPU still reports
    // NVIDIA present here.
    bool nvidia_available = false;
    // The NVIDIA element factories actually installed (e.g. "nvh265enc",
    // "nvh264dec"), in the fixed discovery order below. Empty when none found.
    std::vector<std::string> nvidia_elements;
};

// Whether `renderer` names an NVIDIA GPU (case-insensitive "nvidia" substring,
// e.g. "NVIDIA GeForce RTX 3060/PCIe/SSE2"). Pure and header-only so the app's
// engine-neutral controller can classify without linking GStreamer.
inline bool is_nvidia_renderer(std::string_view renderer)
{
    constexpr std::string_view needle = "nvidia";
    return std::search(renderer.begin(), renderer.end(), needle.begin(), needle.end(),
                       [](const unsigned char a, const unsigned char b) {
                           return std::tolower(a) == std::tolower(b);
                       })
            != renderer.end();
}

// Whether the active GL renderer is not NVIDIA while an NVIDIA GPU is present -
// the hybrid/PRIME mismatch the indicator calls out.
inline bool nvidia_mismatch(std::string_view renderer, bool nvidia_available)
{
    return nvidia_available && !is_nvidia_renderer(renderer);
}

// The launch hint for that mismatch, or empty when not applicable (no NVIDIA,
// or the GL renderer already is NVIDIA). Single source of truth shared by the
// startup log and the GPU indicator's tooltip.
inline std::string nvidia_offload_hint(std::string_view renderer, bool nvidia_available)
{
    if (!nvidia_mismatch(renderer, nvidia_available)) {
        return { };
    }
    std::string hint = "NVIDIA available — GL rendering on '";
    hint += renderer;
    hint += "'; launch with `__NV_PRIME_RENDER_OFFLOAD=1 "
            "__GLX_VENDOR_LIBRARY_NAME=nvidia` to render on NVIDIA";
    return hint;
}

// Whether `renderer` names a software rasteriser rather than a GPU. Case-
// insensitive substring match over the canonical software renderer names.
bool is_software_renderer(std::string_view renderer);

// Whether any ranked hardware decoder exists (registry inspection only - the
// same criterion the decode policy's Hardware answer uses - so no device is
// opened). The probe reads it into HardwareInfo::hardware_decode; the session
// re-reads it when a preference change needs the same answer without the full
// GL/encoder probe.
bool hardware_decode_available();

// Whether the element factory `name` is installed AND actually reachable: it is
// created and driven to READY, which is where a hardware element opens its
// device and a present-but-broken factory (e.g. nvh265enc with no NVIDIA GPU)
// fails. Bounded to one NULL->READY->NULL round trip.
bool element_usable(const char *name);

// The NVIDIA element factories installed on this machine (a registry lookup,
// not a READY probe). Presence of the nvenc/nvdec/nvcuda/nvv4l2 families
// signals an NVIDIA driver/stack, independent of which GPU the GL context
// landed on. Reported in a fixed order so the list is stable.
std::vector<std::string> nvidia_elements_present();

// Whether an /dev/nvidia* device node exists (nvidia0, nvidiactl, ...). A
// driver can be loaded with no GStreamer NVIDIA plugin installed; either signal
// counts as NVIDIA present.
bool nvidia_device_nodes_present();

// Probes this machine's video hardware. Best-effort and bounded: the GL
// renderer is read back from a throwaway offscreen context, the decoder check
// inspects the registry, and the encoder check drives the hardware encoder
// candidates to READY. Never blocks on a real render or a network.
HardwareInfo probe_hardware();

} // namespace genesis::adapters::engine::ges
