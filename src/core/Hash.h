// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// FNV-1a 64 and its sixteen-hex-digit rendering. Pinned: cache filenames and
// effect fingerprints are identities, so changing the offset basis, the prime
// or the digest width would orphan written caches and repaint identical graphs.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace genesis::core {

inline std::uint64_t fnv1a(std::span<const std::uint8_t> bytes)
{
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 0x00000100000001b3ULL;
    }
    return hash;
}

inline std::uint64_t fnv1a(std::string_view text)
{
    return fnv1a(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(text.data()),
                                               text.size()));
}

inline std::string hex16(std::uint64_t value)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int index = 15; index >= 0; --index) {
        out[static_cast<std::size_t>(index)] = kDigits[value & 0xf];
        value >>= 4;
    }
    return out;
}

} // namespace genesis::core
