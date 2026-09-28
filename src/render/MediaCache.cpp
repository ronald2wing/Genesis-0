// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "render/MediaCache.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "core/Bytes.h"
#include "core/CacheFiles.h"
#include "core/FileBytes.h"
#include "core/Hash.h"

namespace genesis::render {

namespace {

using core::cache_sidecar_path;
using core::fnv1a;
using core::hex16;
using core::read_binary_file;
using core::read_le;

// ---- On-disk key format ----
//
// A small binary sidecar, not an embedded header: the waveform file's byte
// layout is frozen (Peaks::encode) and the image caches must stay images, so
// the key cannot live inside any of them. Every integer is little-endian.
//
//   offset  size  field
//   0       4     magic "GK01"
//   4       1     version = 1
//   5       8     source_size (u64)
//   13      8     cache_size  (u64) - the data file's size when recorded
//   21      4     source_len  (u32)
//   25      n     source      (UTF-8 path bytes)
//   25+n    4     buckets_per_second (u32)
//   29+n    4     columns            (u32)
//   33+n    4     tile_width         (u32)
//   37+n    8     start              (f64)
//   45+n    8     duration           (f64)
//   53+n    8     at                 (f64)
//
// A decode that sees the wrong magic, the wrong version, or a byte count that
// does not equal 61 + source_len refuses - so a truncated or hand-edited key
// reads stale rather than crashing the reader.

constexpr std::array<std::uint8_t, 4> kMagic = { 'G', 'K', '0', '1' };
constexpr std::uint8_t kVersion = 1;

// A decoded sidecar: the key it recorded plus the data file's size at the
// time, which is how a later truncation is detected.
struct RecordedKey
{
    CacheKey key;
    std::uintmax_t cache_size = 0;
};

std::vector<std::uint8_t> encode_key(const CacheKey &key, std::uintmax_t cache_size)
{
    const std::string source = key.source.string();
    // Four magic bytes, one version, two 64-bit sizes, a 32-bit path length,
    // the path, then the shaping parameters: three 32-bit counts, three
    // doubles. Sized in one go and written at a moving offset rather than
    // appended piecewise, because a vector grown in steps defeats GCC's
    // overflow analysis - at -O3 it cannot prove the bound and refuses the
    // translation unit under -Werror.
    constexpr std::size_t kFixed = 61;
    std::vector<std::uint8_t> bytes(kFixed + source.size());
    std::size_t at = 0;
    const auto put = [&bytes, &at](const void *data, std::size_t count) {
        std::memcpy(bytes.data() + at, data, count);
        at += count;
    };
    const auto put_le = [&put](auto value) {
        const auto raw = std::bit_cast<std::array<std::uint8_t, sizeof(value)>>(value);
        put(raw.data(), raw.size());
    };
    put(kMagic.data(), kMagic.size());
    put_le(kVersion);
    put_le(static_cast<std::uint64_t>(key.source_size));
    put_le(static_cast<std::uint64_t>(cache_size));
    put_le(static_cast<std::uint32_t>(source.size()));
    put(source.data(), source.size());
    put_le(key.buckets_per_second);
    put_le(key.columns);
    put_le(key.tile_width);
    put_le(key.start);
    put_le(key.duration);
    put_le(key.at);
    return bytes;
}

std::optional<RecordedKey> decode_key(std::span<const std::uint8_t> bytes)
{
    constexpr std::size_t kHeader = 25;
    if (bytes.size() < kHeader) {
        return std::nullopt;
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin()) || bytes[4] != kVersion) {
        return std::nullopt;
    }
    const std::uint32_t source_len = read_le<std::uint32_t>(bytes.subspan(21, 4));
    const std::size_t n = static_cast<std::size_t>(source_len);
    if (bytes.size() != kHeader + n + 36) {
        return std::nullopt;
    }
    RecordedKey out;
    out.key.source_size = read_le<std::uint64_t>(bytes.subspan(5, 8));
    out.cache_size = read_le<std::uint64_t>(bytes.subspan(13, 8));
    out.key.source = std::filesystem::path(
            std::string(bytes.begin() + static_cast<std::ptrdiff_t>(kHeader),
                        bytes.begin() + static_cast<std::ptrdiff_t>(kHeader + n)));
    std::size_t at = kHeader + n;
    out.key.buckets_per_second = read_le<std::uint32_t>(bytes.subspan(at, 4));
    out.key.columns = read_le<std::uint32_t>(bytes.subspan(at + 4, 4));
    out.key.tile_width = read_le<std::uint32_t>(bytes.subspan(at + 8, 4));
    out.key.start = read_le<double>(bytes.subspan(at + 12, 8));
    out.key.duration = read_le<double>(bytes.subspan(at + 20, 8));
    out.key.at = read_le<double>(bytes.subspan(at + 28, 8));
    return out;
}

} // namespace

std::string cache_stem(std::span<const std::uint8_t> bytes)
{
    return hex16(fnv1a(bytes));
}

std::string cache_stem_for_path(const std::filesystem::path &source)
{
    const std::string path_bytes = source.string();
    const std::vector<std::uint8_t> bytes(path_bytes.begin(), path_bytes.end());
    return cache_stem(bytes);
}

std::uintmax_t cache_source_size(const std::filesystem::path &source)
{
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(source, ec);
    return ec ? 0 : size;
}

CachePaths cache_paths(const std::filesystem::path &cache_root, const std::filesystem::path &source)
{
    const std::string stem = cache_stem_for_path(source);
    return CachePaths{
        cache_root / (stem + ".peaks"),
        cache_root / (stem + ".filmstrip.jpg"),
        cache_root / (stem + ".poster.jpg"),
    };
}

bool cache_record(const std::filesystem::path &cache_file, const CacheKey &key)
{
    std::error_code ec;
    if (!std::filesystem::exists(cache_file, ec)) {
        return false;
    }
    const std::uintmax_t size = std::filesystem::file_size(cache_file, ec);
    if (ec) {
        return false;
    }

    const std::filesystem::path sidecar = cache_sidecar_path(cache_file);
    const std::filesystem::path parent = sidecar.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return false;
        }
    }

    const std::vector<std::uint8_t> bytes = encode_key(key, size);
    std::ofstream out(sidecar, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

bool cache_is_fresh(const std::filesystem::path &cache_file, const CacheKey &key)
{
    std::error_code ec;
    if (!std::filesystem::exists(cache_file, ec)) {
        return false;
    }
    const std::uintmax_t size = std::filesystem::file_size(cache_file, ec);
    if (ec) {
        return false;
    }

    std::vector<std::uint8_t> bytes;
    if (!read_binary_file(cache_sidecar_path(cache_file), bytes)) {
        return false;
    }
    const std::optional<RecordedKey> recorded = decode_key(bytes);
    if (!recorded) {
        return false;
    }
    return recorded->key == key && recorded->cache_size == size;
}

} // namespace genesis::render
