// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <array>
#include <span>
#include <string>

namespace genesis::adapters::engine::ges {

// One encoder element a codec family can answer through, in preference order.
// The host names a family; the engine names the element. Each candidate carries
// the raw pixel format its sink accepts for eight- and ten-bit input, so the
// render can pin the raw format a ten-bit request needs: a null ten-bit format
// means the element cannot encode ten bits and must not answer a ten-bit
// request. `hardware` separates the GPU encoders the status probe reports from
// the software fallback the exporter also knows about.
struct EncoderCandidate
{
    const char *element; // element factory name
    const char *format_8; // raw format for 8-bit, e.g. "I420"; null = negotiate
    const char *format_10; // raw format for 10-bit, e.g. "I420_10LE"; null = unsupported
    bool hardware; // a GPU encoder, not a software fallback
};

// The HEVC encoders the exporter can reach, in preference order: the hardware
// encoders first (vendor-specific GPU encoders), then the software x265
// fallback. The single source of truth shared by the exporter (which picks the
// encoder) and the probe (which reports whether hardware encode is reachable),
// so the indicator always agrees with what export would actually pick.
//
// Every entry must be pinnable: the exporter pins its choice with
// `gst_encoding_profile_set_preset_name`, and encodebin only honours that name
// for factories ranked GST_RANK_MARGINAL or above (its encoder list is built
// from that floor). nvautogpuh265enc (NVIDIA's auto-GPU dispatcher) is rank
// NONE, so it can never be pinned and is deliberately absent; nvh265enc is the
// same NVENC backend and ranks primary + 1.
inline constexpr std::array<EncoderCandidate, 5> kH265Encoders = { {
        { "nvh265enc", "NV12", "P010_10LE", true },
        { "vaapih265enc", "NV12", "P010_10LE", true },
        { "vah265enc", "NV12", "P010_10LE", true },
        { "qsvh265enc", "NV12", "P010_10LE", true },
        { "x265enc", "I420", "I420_10LE", false },
} };

// The candidate `candidates` answers with on this machine, or nullptr. Walks
// the candidates in preference order and returns the first whose element
// factory is `usable` and, for a ten-bit request, whose sink `accepts` the
// candidate's ten-bit format. `usable(name)` and `accepts(name, format)` are
// injected so the preference logic is testable without a registry; the exporter
// binds the real registry, the probe binds a READY probe. `family_name` names
// the codec for a legible refusal. On success `raw_format` is the raw format
// the render must pin (null when the encoder negotiates freely); on failure
// `error` names the refusal.
template <typename UsableFn, typename AcceptsFn>
const EncoderCandidate *
resolve_encoder(std::span<const EncoderCandidate> candidates, bool ten_bit, const char **raw_format,
                std::string &error, const char *family_name, UsableFn &&usable, AcceptsFn &&accepts)
{
    for (const EncoderCandidate &candidate : candidates) {
        if (!usable(candidate.element)) {
            continue;
        }
        if (ten_bit
            && (candidate.format_10 == nullptr
                || !accepts(candidate.element, candidate.format_10))) {
            // Installed, but cannot answer a ten-bit request; try the next.
            continue;
        }
        *raw_format = ten_bit ? candidate.format_10 : candidate.format_8;
        return &candidate;
    }
    if (ten_bit) {
        error = std::string("codec unavailable: no installed ") + family_name
                + " encoder supports 10-bit on this machine";
    } else {
        error = std::string("codec unavailable: no ") + family_name + " encoder is installed";
    }
    return nullptr;
}

// Whether the element factory `element`'s sink pad template accepts raw video
// in `format`. The candidate table names each element's ten-bit format; this is
// the defensive re-check that refuses a mis-declared element rather than
// letting encodebin negotiate a silent 8-bit fallback.
bool sink_accepts(const char *element, const char *format);

} // namespace genesis::adapters::engine::ges
