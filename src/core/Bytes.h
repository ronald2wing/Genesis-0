// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Little-endian reads and writes for the binary sidecars the caches keep next
// to their media. The on-disk layout is defined in little-endian order, so the
// host's byte order is normalised here rather than at every call site. Sizes
// are the caller's contract: a read past the end of the span is undefined, as
// it was when each cache carried its own copy.

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace genesis::core {

template <typename T>
T read_le(std::span<const std::uint8_t> bytes, std::size_t at = 0)
{
    std::array<std::uint8_t, sizeof(T)> raw{ };
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(at), sizeof(T), raw.begin());
    if constexpr (std::endian::native == std::endian::big) {
        std::reverse(raw.begin(), raw.end());
    }
    return std::bit_cast<T>(raw);
}

template <typename T>
void append_le(std::vector<std::uint8_t> &out, T value)
{
    const auto raw = std::bit_cast<std::array<std::uint8_t, sizeof(T)>>(value);
    if constexpr (std::endian::native == std::endian::big) {
        out.insert(out.end(), raw.rbegin(), raw.rend());
    } else {
        out.insert(out.end(), raw.begin(), raw.end());
    }
}

} // namespace genesis::core
