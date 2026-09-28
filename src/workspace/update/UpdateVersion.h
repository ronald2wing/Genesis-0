// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The dotted-version comparison shared by the notify-only update check and the
// self-updater's monotonic anti-downgrade/replay gate. It lives in the host so
// both sides of that gate (the app's "is there something newer" surface and the
// updater's "may this be applied" check) share one saturating implementation;
// the app re-exports it from app/Version.h as genesis::app::compare_versions.

#pragma once

#include <cstdint>
#include <limits>
#include <string_view>

namespace genesis::workspace::update {

// Compares two dotted numeric versions ("1.2.3" vs "0.1.0"). Returns -1, 0 or
// 1. A missing trailing component reads as zero ("1.2" == "1.2.0"); any
// non-numeric run (a "v" prefix, a "-beta" suffix) is skipped rather than
// parsed, so a "v1.2.3" still compares as 1.2.3.
inline int compare_versions(std::string_view lhs, std::string_view rhs)
{
    std::size_t li = 0;
    std::size_t ri = 0;

    const auto component = [](std::string_view text, std::size_t &i) {
        // Skip whatever is not part of a numeric component (a "v" prefix, say);
        // a component with no digits reads zero.
        while (i < text.size() && text[i] != '.' && (text[i] < '0' || text[i] > '9')) {
            ++i;
        }
        std::uint64_t value = 0;
        constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
        while (i < text.size() && text[i] != '.' && text[i] >= '0' && text[i] <= '9') {
            const std::uint64_t digit = static_cast<std::uint64_t>(text[i] - '0');
            // Saturate on overflow: a component longer than a uint64 holds
            // still reads "huge" rather than wrapping to a small value, so a
            // ridiculous version never compares as older than a sane one.
            value = value > (kMax - digit) / 10 ? kMax : value * 10 + digit;
            ++i;
        }
        if (i < text.size() && text[i] == '.') {
            ++i;
        }
        return value;
    };

    while (li < lhs.size() || ri < rhs.size()) {
        const std::uint64_t l = component(lhs, li);
        const std::uint64_t r = component(rhs, ri);
        if (l != r) {
            return l < r ? -1 : 1;
        }
    }
    return 0;
}

} // namespace genesis::workspace::update
