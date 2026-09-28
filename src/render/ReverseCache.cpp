// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "render/ReverseCache.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace genesis::render {

namespace {

// The reverse stem folds the source path and both span doubles, so two spans
// of one source hash apart. Each double is folded in as its exact 64-bit
// little-endian bit pattern, so spans that round to the same double share a
// file (and its recorded key) while spans that differ at all do not.
std::string reverse_stem(const std::filesystem::path &source, double start, double duration)
{
    const std::string path = source.string();
    std::vector<std::uint8_t> bytes;
    bytes.reserve(path.size() + 16);
    bytes.insert(bytes.end(), path.begin(), path.end());
    const auto append = [&bytes](double value) {
        const auto raw = std::bit_cast<std::array<std::uint8_t, 8>>(value);
        bytes.insert(bytes.end(), raw.begin(), raw.end());
    };
    append(start);
    append(duration);
    return cache_stem(bytes);
}

} // namespace

std::filesystem::path reverse_cache_path(const std::filesystem::path &cache_root,
                                         const std::filesystem::path &source, double start,
                                         double duration)
{
    return cache_root / (reverse_stem(source, start, duration) + ".reverse.webm");
}

CacheKey reverse_key(const std::filesystem::path &source, std::uintmax_t source_size, double start,
                     double duration)
{
    CacheKey key;
    key.source = source;
    key.source_size = source_size;
    key.start = start;
    key.duration = duration;
    return key;
}

} // namespace genesis::render
