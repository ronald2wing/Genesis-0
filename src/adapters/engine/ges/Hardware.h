// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <string>
#include <string_view>

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
};

// Whether `renderer` names a software rasteriser rather than a GPU. Case-
// insensitive substring match over the canonical software renderer names.
bool is_software_renderer(std::string_view renderer);

// Whether the element factory `name` is installed AND actually reachable: it is
// created and driven to READY, which is where a hardware element opens its
// device and a present-but-broken factory (e.g. nvh265enc with no NVIDIA GPU)
// fails. Bounded to one NULL->READY->NULL round trip.
bool element_usable(const char *name);

// Probes this machine's video hardware. Best-effort and bounded: the GL
// renderer is read back from a throwaway offscreen context, the decoder check
// inspects the registry, and the encoder check drives the hardware encoder
// candidates to READY. Never blocks on a real render or a network.
HardwareInfo probe_hardware();

} // namespace genesis::adapters::engine::ges
