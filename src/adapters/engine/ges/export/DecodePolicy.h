// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "adapters/engine/EngineSession.h"
#include "adapters/engine/ges/export/Hardware.h"

namespace genesis::adapters::engine::ges {

// Which operating system the decode policy targets. The compile-time mapping is
// current_platform(); tests pass a platform explicitly so the preference x
// platform x availability matrix runs on the one machine this tree is built on.
enum class DecodePlatform { Linux, Macos, Windows };

DecodePlatform current_platform();

// The rank constants the policy plans in, mirroring GST_RANK_*. Kept as plain
// numbers so the pure planning function needs no GStreamer header; the adapter
// static_asserts the correspondence against GST_RANK_* in DecodePolicy.cpp.
inline constexpr std::uint32_t kRankNone = 0;
inline constexpr std::uint32_t kRankMarginal = 64;
inline constexpr std::uint32_t kRankSecondary = 128;
inline constexpr std::uint32_t kRankPrimary = 256;
// Where the policy parks a promoted hardware decoder: one notch above the
// software default (PRIMARY) so decodebin's autoplugging prefers it on ties.
inline constexpr std::uint32_t kHardwareRank = kRankPrimary + 1;

// One decoder in the registry snapshot the policy plans over: its element name,
// whether its factory is classified "Hardware" (klass "/Hardware" suffix), and
// its current rank (kRankNone when the distro has disabled it).
struct DecoderEntry
{
    std::string name;
    bool hardware = false;
    std::uint32_t rank = kRankNone;
};

// A single rank change the policy decided: the element and the rank to set.
struct RankChange
{
    std::string name;
    std::uint32_t rank = kRankNone;
};

// The pure policy decision for a registry snapshot. `changes` is empty when the
// policy leaves the snapshot as-is (Auto with nothing to promote, for example).
// `honoured` is false only when Hardware was asked for and no hardware decoder
// exists. `path` is the effective decode path ("hardware"/"software") for the
// status surface.
struct DecodePlan
{
    std::vector<RankChange> changes;
    bool honoured = true;
    std::string path;
};

// Maps a platform, a preference and a decoder snapshot to a rank-change plan.
// Pure: same inputs -> same plan, and no registry is touched, so the
// preference x platform x availability matrix is unit-testable without GStreamer.
DecodePlan plan_decode_policy(DecodePlatform platform, DecodePreference preference,
                              const std::vector<DecoderEntry> &decoders);

// The registry seam the policy reads and writes. The engine binds the real
// GStreamer registry (GstDecodeRegistry); tests bind a fake so the rank changes
// can be asserted without a plugin set.
class DecodeRegistry
{
public:
    virtual ~DecodeRegistry() = default;
    // The decoder snapshot: every decoder element, its hardware flag and rank.
    virtual std::vector<DecoderEntry> decoders() const = 0;
    // The current rank of `name` (kRankNone when absent).
    virtual std::uint32_t rank(const std::string &name) const = 0;
    // Sets `name`'s rank (no-op when absent).
    virtual void set_rank(const std::string &name, std::uint32_t rank) = 0;
};

// The real GStreamer registry: decoder enumeration via the element-factory
// list, ranks via gst_plugin_feature_get/set_rank.
class GstDecodeRegistry final : public DecodeRegistry
{
public:
    std::vector<DecoderEntry> decoders() const override;
    std::uint32_t rank(const std::string &name) const override;
    void set_rank(const std::string &name, std::uint32_t rank) override;
};

struct DecodePolicyResult
{
    bool honoured = true;
    std::string path;
};

// Applies the decode policy to the process-global GStreamer registry. Records
// the ranks it changes so a later re-apply (a preference change) restores them
// before re-tilting. `probed` is the probe result whose `hardware_decode` is the
// authoritative "hardware exists" signal, keeping the policy's answer and the
// status surface consistent.
DecodePolicyResult apply_decode_policy(DecodePreference preference, const HardwareInfo &probed);

// The same over an injected registry (the tests' entry point).
DecodePolicyResult apply_decode_policy(DecodeRegistry &registry, DecodePreference preference,
                                       const HardwareInfo &probed);

// Restores every rank this process has tilted to its shipped value and forgets
// them, leaving the registry exactly as the engine shipped it.
// apply_decode_policy calls this before re-tilting; it is public so tests (and
// any future teardown) can assert the restore in isolation.
void restore_decode_policy(DecodeRegistry &registry);

} // namespace genesis::adapters::engine::ges
