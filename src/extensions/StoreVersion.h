// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The version directory name, as a number. A non-numeric name (a store someone
// edited by hand) counts as 0 so it never wins over a real version. Both the
// extension host's scan and the manager's newest-version lookup order stores by
// this number, so the rule lives once.

#pragma once

#include <charconv>
#include <cstdint>
#include <limits>
#include <string_view>
#include <system_error>

namespace genesis::extensions {

inline std::uint32_t parse_version(std::string_view text)
{
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{ }) {
        return 0;
    }
    return value > std::numeric_limits<std::uint32_t>::max()
            ? std::numeric_limits<std::uint32_t>::max()
            : static_cast<std::uint32_t>(value);
}

} // namespace genesis::extensions
