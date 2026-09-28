// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "adapters/engine/ges/export/DecodePolicy.h"

#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <gst/gst.h>

namespace genesis::adapters::engine::ges {

namespace {

using namespace std::string_view_literals;

static_assert(kRankNone == GST_RANK_NONE, "kRankNone tracks GST_RANK_NONE");
static_assert(kRankMarginal == GST_RANK_MARGINAL, "kRankMarginal tracks GST_RANK_MARGINAL");
static_assert(kRankSecondary == GST_RANK_SECONDARY, "kRankSecondary tracks GST_RANK_SECONDARY");
static_assert(kRankPrimary == GST_RANK_PRIMARY, "kRankPrimary tracks GST_RANK_PRIMARY");

// Whether a decoder factory is hardware-accelerated. Mirrors the classification
// the probe acts on (Hardware.cpp): the engine marks hardware decoders with a
// "/Hardware" suffix in their klass, and that is the single source of truth for
// "can this machine decode in hardware".
bool is_hardware_decoder(GstElementFactory *factory)
{
    const gchar *klass = gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS);
    return klass != nullptr && g_str_has_suffix(klass, "Hardware");
}

// The exact element names this platform's policy promotes.
std::span<const std::string_view> platform_exact_elements(DecodePlatform platform)
{
    static constexpr std::array kLinux = { "va"sv, "vah264dec"sv, "vah265dec"sv,
                                           "vaapidecodebin"sv };
    static constexpr std::array kMacos = { "vtdec"sv };
    switch (platform) {
    case DecodePlatform::Linux:
        return kLinux;
    case DecodePlatform::Macos:
        return kMacos;
    case DecodePlatform::Windows:
        return { };
    }
    return { };
}

// The element-name prefixes this platform's policy promotes.
std::span<const std::string_view> platform_prefixes(DecodePlatform platform)
{
    static constexpr std::array kWindows = { "d3d11"sv, "mf"sv };
    switch (platform) {
    case DecodePlatform::Windows:
        return kWindows;
    case DecodePlatform::Linux:
    case DecodePlatform::Macos:
        return { };
    }
    return { };
}

// Whether `name` is a decoder this platform's policy promotes: an exact match
// or a prefix match in the platform list, or the NVIDIA `nv` prefix (promoted
// on every platform where NVIDIA decoders are present).
bool is_platform_decoder(DecodePlatform platform, std::string_view name)
{
    for (const std::string_view exact : platform_exact_elements(platform)) {
        if (name == exact) {
            return true;
        }
    }
    for (const std::string_view prefix : platform_prefixes(platform)) {
        if (name.starts_with(prefix)) {
            return true;
        }
    }
    return name.starts_with("nv");
}

// The shipped rank of every feature this process has tilted, keyed by element
// name and recorded on first touch. Re-found by name on restore rather than
// kept as a pointer: the registry owns the factory and may re-scan plugins.
std::unordered_map<std::string, std::uint32_t> &original_ranks()
{
    static std::unordered_map<std::string, std::uint32_t> ranks;
    return ranks;
}

} // namespace

void restore_decode_policy(DecodeRegistry &registry)
{
    for (const auto &[name, rank] : original_ranks()) {
        registry.set_rank(name, rank);
    }
    original_ranks().clear();
}

DecodePlatform current_platform()
{
#if defined(__APPLE__)
    return DecodePlatform::Macos;
#elif defined(_WIN32)
    return DecodePlatform::Windows;
#else
    return DecodePlatform::Linux;
#endif
}

DecodePlan plan_decode_policy(DecodePlatform platform, DecodePreference preference,
                              const std::vector<DecoderEntry> &decoders)
{
    DecodePlan plan;

    // "Hardware exists" = any hardware decoder the distro has not disabled
    // (rank != NONE). This is the same criterion the probe's hardware_decode
    // reads, so the plan's honoured/path answer and the probe's report agree.
    const bool any_hardware =
            std::any_of(decoders.begin(), decoders.end(),
                        [](const DecoderEntry &d) { return d.hardware && d.rank != kRankNone; });

    switch (preference) {
    case DecodePreference::Software:
        // Force the software path: demote every ranked hardware decoder, so
        // decodebin has no hardware candidate left to autoplug. Always honoured
        // (software decode always exists).
        for (const DecoderEntry &d : decoders) {
            if (d.hardware && d.rank != kRankNone) {
                plan.changes.push_back(RankChange{ d.name, kRankNone });
            }
        }
        break;
    case DecodePreference::Auto:
        // Modest promotion, close to the engine defaults: lift present, ranked
        // platform decoders one notch above the software default, leaving every
        // other rank (software and non-platform hardware) as shipped.
        for (const DecoderEntry &d : decoders) {
            if (!is_platform_decoder(platform, d.name)) {
                continue;
            }
            if (d.rank == kRankNone) {
                continue; // distro-disabled; never resurrect
            }
            if (d.rank != kHardwareRank) {
                plan.changes.push_back(RankChange{ d.name, kHardwareRank });
            }
        }
        break;
    case DecodePreference::Hardware:
        // Strong promotion: the platform promotion above, plus every ranked
        // software decoder drops below it, so a hardware decoder present for
        // the codec decisively wins autoplugging. Honoured only when hardware
        // exists.
        for (const DecoderEntry &d : decoders) {
            if (is_platform_decoder(platform, d.name)) {
                if (d.rank != kRankNone && d.rank != kHardwareRank) {
                    plan.changes.push_back(RankChange{ d.name, kHardwareRank });
                }
                continue;
            }
            if (!d.hardware && d.rank > kRankSecondary) {
                plan.changes.push_back(RankChange{ d.name, kRankSecondary });
            }
        }
        plan.honoured = any_hardware;
        break;
    }

    plan.path =
            (preference == DecodePreference::Software || !any_hardware) ? "software" : "hardware";
    return plan;
}

std::vector<DecoderEntry> GstDecodeRegistry::decoders() const
{
    std::vector<DecoderEntry> out;
    GList *decoders =
            gst_element_factory_list_get_elements(GST_ELEMENT_FACTORY_TYPE_DECODER, GST_RANK_NONE);
    for (GList *it = decoders; it != nullptr; it = it->next) {
        auto *factory = GST_ELEMENT_FACTORY(it->data);
        DecoderEntry entry;
        entry.name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
        entry.hardware = is_hardware_decoder(factory);
        entry.rank = gst_plugin_feature_get_rank(GST_PLUGIN_FEATURE(factory));
        out.push_back(std::move(entry));
    }
    gst_plugin_feature_list_free(decoders);
    return out;
}

std::uint32_t GstDecodeRegistry::rank(const std::string &name) const
{
    GstElementFactory *factory = gst_element_factory_find(name.c_str());
    if (factory == nullptr) {
        return kRankNone;
    }
    const std::uint32_t result = gst_plugin_feature_get_rank(GST_PLUGIN_FEATURE(factory));
    gst_object_unref(factory);
    return result;
}

void GstDecodeRegistry::set_rank(const std::string &name, std::uint32_t rank)
{
    GstElementFactory *factory = gst_element_factory_find(name.c_str());
    if (factory == nullptr) {
        return;
    }
    gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(factory), rank);
    gst_object_unref(factory);
}

DecodePolicyResult apply_decode_policy(DecodeRegistry &registry, DecodePreference preference,
                                       const HardwareInfo &probed)
{
    // Start from the shipped ranks so a preference change re-tilts from a known
    // baseline rather than stacking.
    restore_decode_policy(registry);

    const std::vector<DecoderEntry> decoders = registry.decoders();
    DecodePlan plan = plan_decode_policy(current_platform(), preference, decoders);

    // The probe's hardware_decode is the authoritative "hardware exists" signal;
    // prefer it over the snapshot so the policy's answer and the status surface
    // can never disagree.
    if (preference == DecodePreference::Hardware) {
        plan.honoured = probed.hardware_decode;
    }

    for (const RankChange &change : plan.changes) {
        original_ranks().emplace(change.name, registry.rank(change.name));
        registry.set_rank(change.name, change.rank);
    }
    return DecodePolicyResult{ plan.honoured, std::move(plan.path) };
}

DecodePolicyResult apply_decode_policy(DecodePreference preference, const HardwareInfo &probed)
{
    GstDecodeRegistry registry;
    return apply_decode_policy(registry, preference, probed);
}

} // namespace genesis::adapters::engine::ges
