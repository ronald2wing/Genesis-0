// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

// The encoder-selection suite: the shared HEVC candidate table's preference
// order and hardware flags, and the injected-predicate resolver. Pure and
// registry-free: resolve_encoder takes usable/accepts lambdas, so no GStreamer
// registry is needed here.

#include <cstdio>
#include <span>
#include <string>
#include <string_view>

#include "adapters/engine/ges/Encoder.h"

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

namespace ges = genesis::adapters::engine::ges;

void test_k_h265_encoders_order()
{
    // The shared hardware-first list: five candidates, the four GPU encoders in
    // preference order, then the software x265 fallback. The exporter walks this
    // same list, so the order and the hardware flag are a contract.
    check(ges::kH265Encoders.size() == 5, "kH265Encoders has five candidates");
    const char *const expected[5] = { "nvh265enc", "vaapih265enc", "vah265enc", "qsvh265enc",
                                      "x265enc" };
    for (std::size_t i = 0; i < ges::kH265Encoders.size(); ++i) {
        check(std::string_view(ges::kH265Encoders[i].element) == expected[i],
              "candidate order matches the preference order");
        const bool is_hardware = i < 4;
        check(ges::kH265Encoders[i].hardware == is_hardware,
              "hardware flag marks the GPU encoders, not the software fallback");
        check(ges::kH265Encoders[i].format_10 != nullptr, "every candidate names a ten-bit format");
    }
}

void test_resolve_encoder()
{
    const std::span<const ges::EncoderCandidate> candidates =
            std::span<const ges::EncoderCandidate>(ges::kH265Encoders);
    const char *raw = nullptr;
    std::string error;

    // Functional: every element usable and every ten-bit sink accepts, so the
    // first (hardware) candidate answers and the raw format matches the depth.
    const auto all_usable = [](const char *) { return true; };
    const auto all_accepts = [](const char *, const char *) { return true; };
    const ges::EncoderCandidate *eight =
            ges::resolve_encoder(candidates, false, &raw, error, "hevc", all_usable, all_accepts);
    check(eight != nullptr, "a usable encoder answers an 8-bit request");
    check(eight != nullptr && std::string_view(eight->element) == "nvh265enc",
          "the first candidate answers in preference order");
    check(raw != nullptr && std::string_view(raw) == "NV12", "the 8-bit raw format is pinned");
    const ges::EncoderCandidate *ten =
            ges::resolve_encoder(candidates, true, &raw, error, "hevc", all_usable, all_accepts);
    check(ten != nullptr && std::string_view(ten->element) == "nvh265enc",
          "the first candidate answers a 10-bit request");
    check(raw != nullptr && std::string_view(raw) == "P010_10LE",
          "the 10-bit raw format is pinned");

    // Unavailable: nothing usable, so the resolver refuses with the family name
    // and says no encoder is installed.
    const auto none_usable = [](const char *) { return false; };
    const ges::EncoderCandidate *none =
            ges::resolve_encoder(candidates, false, &raw, error, "hevc", none_usable, all_accepts);
    check(none == nullptr, "no usable encoder refuses");
    check(error.find("hevc") != std::string::npos, "the refusal names the family");
    check(error.find("is installed") != std::string::npos,
          "the refusal says no encoder is installed");

    // Broken: a present (usable) hardware encoder whose sink refuses the
    // ten-bit format is skipped, and with no other candidate the request
    // refuses rather than silently downgrading to 8 bits.
    const auto only_first_usable = [](const char *name) {
        return std::string_view(name) == "nvh265enc";
    };
    const auto refuse_all = [](const char *, const char *) { return false; };
    const ges::EncoderCandidate *broken = ges::resolve_encoder(
            candidates, true, &raw, error, "hevc", only_first_usable, refuse_all);
    check(broken == nullptr, "a usable-but-broken ten-bit encoder refuses");
    check(error.find("10-bit") != std::string::npos,
          "the refusal names the missing ten-bit support");
}

} // namespace

int main()
{
    test_k_h265_encoders_order();
    test_resolve_encoder();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
