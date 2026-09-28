// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Lowercase hex for digests and cache keys. The alphabet is fixed: a digest is
// compared against a recorded one and a cache key is a filename, so changing
// the digits would invalidate both.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace genesis::core {

inline std::string hex_encode(std::span<const std::uint8_t> bytes)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(bytes.size() * 2, '0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        out[2 * index] = kDigits[bytes[index] >> 4];
        out[(2 * index) + 1] = kDigits[bytes[index] & 0xf];
    }
    return out;
}

} // namespace genesis::core
