// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

// The per-platform decode policy's suite. The first half is pure: the
// preference x platform x availability mapping over a fake decoder snapshot,
// with no GStreamer and no registry. The second half binds the real
// GStreamer registry to assert promote/demote/restore against whatever
// decoders this machine actually ships, and to prove the policy's Hardware
// answer agrees with the probe's hardware_decode report.

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ges/ges.h>
#include <gst/gst.h>

#include "adapters/engine/ges/export/DecodePolicy.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

namespace ges = genesis::adapters::engine::ges;
namespace engine = genesis::adapters::engine;

using engine::DecodePreference;

ges::DecoderEntry dec(std::string name, bool hardware, std::uint32_t rank)
{
    ges::DecoderEntry entry;
    entry.name = std::move(name);
    entry.hardware = hardware;
    entry.rank = rank;
    return entry;
}

// The rank the plan assigns `name`, or std::nullopt when the plan leaves it
// untouched.
std::optional<std::uint32_t> change_for(const ges::DecodePlan &plan, std::string_view name)
{
    for (const ges::RankChange &change : plan.changes) {
        if (change.name == name) {
            return change.rank;
        }
    }
    return std::nullopt;
}

// A registry over a fixed in-memory snapshot. Ranks are mutable so the apply
// and restore mechanics can be asserted without a plugin set.
class FakeRegistry final : public ges::DecodeRegistry
{
public:
    std::vector<ges::DecoderEntry> entries;

    std::vector<ges::DecoderEntry> decoders() const override { return entries; }

    std::uint32_t rank(const std::string &name) const override
    {
        for (const ges::DecoderEntry &entry : entries) {
            if (entry.name == name) {
                return entry.rank;
            }
        }
        return ges::kRankNone;
    }

    void set_rank(const std::string &name, std::uint32_t value) override
    {
        for (ges::DecoderEntry &entry : entries) {
            if (entry.name == name) {
                entry.rank = value;
            }
        }
    }
};

// Software forces the escape hatch: every ranked hardware decoder is demoted
// to NONE so decodebin has no hardware candidate left, whatever the platform.
void test_plan_software_demotes_all_hardware()
{
    const std::vector<ges::DecoderEntry> decoders = {
        dec("vah264dec", true, ges::kRankPrimary), // platform hardware
        dec("avdec_h264", false, ges::kRankPrimary), // software
        dec("d3d11h264dec", true, ges::kRankPrimary), // non-platform hardware
        dec("nvh264dec", true, ges::kRankNone), // distro-disabled hardware
    };

    const ges::DecodePlan plan = ges::plan_decode_policy(ges::DecodePlatform::Linux,
                                                         DecodePreference::Software, decoders);

    check(change_for(plan, "vah264dec") == ges::kRankNone, "platform hardware demoted to NONE");
    check(change_for(plan, "d3d11h264dec") == ges::kRankNone,
          "non-platform hardware demoted to NONE");
    check(!change_for(plan, "avdec_h264").has_value(), "software decoder is untouched");
    check(!change_for(plan, "nvh264dec").has_value(), "already-disabled decoder stays disabled");
    check(plan.honoured, "Software is always honoured");
    check(plan.path == "software", "Software reports the software path");
}

// Auto lifts present, ranked platform decoders one notch above the software
// default, leaving everything else as shipped.
void test_plan_auto_promotes_platform_decoders()
{
    const std::vector<ges::DecoderEntry> decoders = {
        dec("vah264dec", true, ges::kRankPrimary),
        dec("vah265dec", true, ges::kRankPrimary),
        dec("vaapidecodebin", true, ges::kRankPrimary),
        dec("nvh264dec", true, ges::kRankPrimary), // universal nv prefix
        dec("avdec_h264", false, ges::kRankPrimary),
        dec("d3d11h264dec", true, ges::kRankPrimary), // not a Linux platform decoder
    };

    const ges::DecodePlan plan =
            ges::plan_decode_policy(ges::DecodePlatform::Linux, DecodePreference::Auto, decoders);

    check(change_for(plan, "vah264dec") == ges::kHardwareRank, "vah264dec promoted");
    check(change_for(plan, "vah265dec") == ges::kHardwareRank, "vah265dec promoted");
    check(change_for(plan, "vaapidecodebin") == ges::kHardwareRank, "vaapidecodebin promoted");
    check(change_for(plan, "nvh264dec") == ges::kHardwareRank, "nv decoder promoted everywhere");
    check(!change_for(plan, "avdec_h264").has_value(), "software rank left as shipped");
    check(!change_for(plan, "d3d11h264dec").has_value(), "non-platform hardware left as shipped");
    check(plan.honoured, "Auto is always honoured");
    check(plan.path == "hardware", "Auto reports hardware when hardware exists");
}

// A distro-disabled (rank NONE) decoder is never resurrected, and a decoder
// already at the promoted rank is not re-recorded.
void test_plan_auto_never_resurrects_disabled()
{
    const std::vector<ges::DecoderEntry> decoders = {
        dec("vah264dec", true, ges::kRankNone), // distro-disabled
        dec("vaapidecodebin", true, ges::kHardwareRank), // already promoted
        dec("avdec_h264", false, ges::kRankPrimary),
    };

    const ges::DecodePlan plan =
            ges::plan_decode_policy(ges::DecodePlatform::Linux, DecodePreference::Auto, decoders);

    check(!change_for(plan, "vah264dec").has_value(), "disabled decoder not resurrected");
    check(!change_for(plan, "vaapidecodebin").has_value(),
          "already-promoted decoder not re-recorded");
    check(!change_for(plan, "avdec_h264").has_value(), "software decoder untouched");
}

// Auto with no ranked hardware decoder reports the software path.
void test_plan_auto_no_hardware_path()
{
    const std::vector<ges::DecoderEntry> decoders = {
        dec("vah264dec", true, ges::kRankNone), // distro-disabled
        dec("avdec_h264", false, ges::kRankPrimary),
    };

    const ges::DecodePlan plan =
            ges::plan_decode_policy(ges::DecodePlatform::Linux, DecodePreference::Auto, decoders);
    check(plan.path == "software", "Auto reports software when no ranked hardware exists");
}

// The platform element names and prefixes are per-platform: a Linux decoder is
// not promoted on Windows or macOS, and the nv prefix is the only universal.
void test_plan_cross_platform_promotion()
{
    const std::vector<ges::DecoderEntry> decoders = {
        dec("vah264dec", true, ges::kRankPrimary), dec("d3d11h264dec", true, ges::kRankPrimary),
        dec("mfh264dec", true, ges::kRankPrimary), dec("vtdec", true, ges::kRankPrimary),
        dec("nvh264dec", true, ges::kRankPrimary),
    };

    const ges::DecodePlan windows =
            ges::plan_decode_policy(ges::DecodePlatform::Windows, DecodePreference::Auto, decoders);
    check(change_for(windows, "d3d11h264dec") == ges::kHardwareRank, "d3d11 promoted on Windows");
    check(change_for(windows, "mfh264dec") == ges::kHardwareRank, "mf promoted on Windows");
    check(change_for(windows, "nvh264dec") == ges::kHardwareRank, "nv promoted on Windows");
    check(!change_for(windows, "vah264dec").has_value(), "va decoder not promoted on Windows");
    check(!change_for(windows, "vtdec").has_value(), "vtdec not promoted on Windows");

    const ges::DecodePlan macos =
            ges::plan_decode_policy(ges::DecodePlatform::Macos, DecodePreference::Auto, decoders);
    check(change_for(macos, "vtdec") == ges::kHardwareRank, "vtdec promoted on macOS");
    check(change_for(macos, "nvh264dec") == ges::kHardwareRank, "nv promoted on macOS");
    check(!change_for(macos, "vah264dec").has_value(), "va decoder not promoted on macOS");
    check(!change_for(macos, "d3d11h264dec").has_value(), "d3d11 not promoted on macOS");
    check(!change_for(macos, "mfh264dec").has_value(), "mf not promoted on macOS");
}

// Hardware promotes platform decoders and demotes ranked software decoders
// below them, and its honoured/path answer tracks hardware availability.
void test_plan_hardware_demotes_software()
{
    const std::vector<ges::DecoderEntry> with_hardware = {
        dec("vah264dec", true, ges::kRankPrimary),
        dec("avdec_h264", false, ges::kRankPrimary),
    };
    const ges::DecodePlan plan = ges::plan_decode_policy(ges::DecodePlatform::Linux,
                                                         DecodePreference::Hardware, with_hardware);

    check(change_for(plan, "vah264dec") == ges::kHardwareRank, "platform hardware promoted");
    check(change_for(plan, "avdec_h264") == ges::kRankSecondary, "software demoted below hardware");
    check(plan.honoured, "Hardware honoured when hardware exists");
    check(plan.path == "hardware", "Hardware reports hardware when it exists");

    const std::vector<ges::DecoderEntry> without_hardware = {
        dec("avdec_h264", false, ges::kRankPrimary),
    };
    const ges::DecodePlan none = ges::plan_decode_policy(
            ges::DecodePlatform::Linux, DecodePreference::Hardware, without_hardware);
    check(!none.honoured, "Hardware not honoured when no hardware exists");
    check(none.path == "software", "Hardware falls back to the software path");
}

// The apply/restore mechanics over a fake registry: apply tilts the ranks and
// records the shipped values, restore puts them back and forgets them.
void test_apply_and_restore_mechanics()
{
    FakeRegistry registry;
    registry.entries = {
        dec("vah264dec", true, ges::kRankPrimary),
        dec("avdec_h264", false, ges::kRankPrimary),
    };
    ges::HardwareInfo probed;
    probed.hardware_decode = true;

    const ges::DecodePolicyResult result =
            ges::apply_decode_policy(registry, DecodePreference::Software, probed);
    check(result.honoured, "apply(Software) reports honoured");
    check(result.path == "software", "apply(Software) reports the software path");
    check(registry.rank("vah264dec") == ges::kRankNone, "hardware rank set to NONE");
    check(registry.rank("avdec_h264") == ges::kRankPrimary, "software rank unchanged");

    ges::restore_decode_policy(registry);
    check(registry.rank("vah264dec") == ges::kRankPrimary, "hardware rank restored");

    // A second apply must start from the shipped rank again, not stack: the
    // Auto promotion lands on PRIMARY (the shipped value), never on the NONE
    // the Software apply left behind.
    const ges::DecodePolicyResult second =
            ges::apply_decode_policy(registry, DecodePreference::Auto, probed);
    check(second.honoured, "apply(Auto) reports honoured");
    check(registry.rank("vah264dec") == ges::kHardwareRank,
          "re-apply promotes from the shipped baseline");
    ges::restore_decode_policy(registry);
}

// The real registry: Software demotes every ranked hardware decoder to NONE
// and restore puts each back exactly as it was, proving the engine round-trip.
void test_on_host_promote_demote_restore()
{
    ges::GstDecodeRegistry registry;
    const std::vector<ges::DecoderEntry> before = registry.decoders();

    std::vector<std::pair<std::string, std::uint32_t>> hardware_ranks;
    for (const ges::DecoderEntry &entry : before) {
        if (entry.hardware && entry.rank != ges::kRankNone) {
            hardware_ranks.emplace_back(entry.name, entry.rank);
        }
    }
    std::printf("host registry: %zu ranked hardware decoders\n", hardware_ranks.size());

    ges::HardwareInfo probed = ges::probe_hardware();
    const ges::DecodePolicyResult result =
            ges::apply_decode_policy(registry, DecodePreference::Software, probed);
    check(result.honoured, "Software honoured on the host");
    check(result.path == "software", "Software reports the software path on the host");

    for (const auto &[name, original] : hardware_ranks) {
        check(registry.rank(name) == ges::kRankNone, name + " demoted to NONE");
    }

    ges::restore_decode_policy(registry);
    for (const auto &[name, original] : hardware_ranks) {
        check(registry.rank(name) == original, name + " restored to its shipped rank");
    }
}

// The policy's Hardware answer must agree with the probe's hardware_decode,
// because the status surface reports the latter.
void test_hardware_honoured_matches_probe()
{
    ges::GstDecodeRegistry registry;
    const ges::HardwareInfo probed = ges::probe_hardware();
    const ges::DecodePolicyResult result =
            ges::apply_decode_policy(registry, DecodePreference::Hardware, probed);
    check(result.honoured == probed.hardware_decode,
          "Hardware honoured agrees with the probe's hardware_decode");
    ges::restore_decode_policy(registry);
}

} // namespace

int main()
{
    // The pure and fake-registry tests need no GStreamer; run them before
    // init so a plugin scan cannot leak into the plan assertions.
    test_plan_software_demotes_all_hardware();
    test_plan_auto_promotes_platform_decoders();
    test_plan_auto_never_resurrects_disabled();
    test_plan_auto_no_hardware_path();
    test_plan_cross_platform_promotion();
    test_plan_hardware_demotes_software();
    test_apply_and_restore_mechanics();

    gst_init(nullptr, nullptr);
    ges_init();

    test_on_host_promote_demote_restore();
    test_hardware_honoured_matches_probe();

    return genesis::test::summary();
}
