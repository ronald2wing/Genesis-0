// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace genesis::render {

// What a cache was made from, and therefore when it stops being usable.
//
// A cache lives beside the source it describes and is keyed by the source's
// path and size plus the parameters that shape the produced bytes. Staleness
// is a comparison, never a guess: a cache whose recorded key does not match
// the one asked for is ignored and regenerated.
//
// The shaping parameters are modelled per cache kind, not as one blob, so a
// waveform, a filmstrip and a poster keyed from the same source cannot be
// confused with one another. Fields that do not apply to a kind stay at their
// default; equality compares the whole struct, so a key records exactly what
// produced one cache file.
struct CacheKey
{
    // The source file the cache was made from.
    std::filesystem::path source;
    // Its size in bytes at cache time - the cheap proxy for "the file
    // changed" (no content hash is computed or stored).
    std::uintmax_t source_size = 0;
    // Waveform: buckets per second (Peaks::buckets_per_second). 0 elsewhere.
    std::uint32_t buckets_per_second = 0;
    // Filmstrip: tiles across, each tile's width, and the sampled window.
    std::uint32_t columns = 0;
    std::uint32_t tile_width = 0;
    double start = 0.0; // seconds into the source
    double duration = 0.0; // 0 = to the end
    // Poster: the instant sampled, in seconds.
    double at = 0.0;

    bool operator==(const CacheKey &) const = default;
};

// The cache files a source maps to under a cache root. Pure naming: no I/O,
// no codecs. The name is an FNV-1a 64 hash of the source path (hex), so a
// path maps to the same files every run and two paths never collide in
// practice. The shaping parameters do not ride in
// the name; they ride in the recorded key (see cache_record/cache_is_fresh).
struct CachePaths
{
    std::filesystem::path waveform;
    std::filesystem::path filmstrip;
    std::filesystem::path poster;
};

CachePaths cache_paths(const std::filesystem::path &cache_root,
                       const std::filesystem::path &source);

// The FNV-1a 64 hash of `bytes` as sixteen lowercase hex digits - the stem a
// cache file is named by. Exposed so a cache kind whose name must fold shaping
// parameters in (a per-span reverse) builds its own stem over the same pinned
// hash without re-deriving the constants. Changing this orphans every cache
// already written.
std::string cache_stem(std::span<const std::uint8_t> bytes);

// The FNV-1a stem for a source path alone - the shared path->bytes->stem step
// behind cache_paths and proxy_cache_path. Changing this orphans every cache
// already written.
std::string cache_stem_for_path(const std::filesystem::path &source);

// The source file's size in bytes, 0 when it cannot be statted. A cache keyed
// to size 0 is never fresh against a real file, so "missing" and "stale"
// collapse to the same safe answer.
std::uintmax_t cache_source_size(const std::filesystem::path &source);

// Writes the key sidecar beside `cache_file`, capturing the cache file's own
// size so a later truncation reads stale. Only the key is recorded; producing
// the cache data bytes is the caller's (or the engine's) business. False when
// the cache file is absent or the sidecar cannot be written.
bool cache_record(const std::filesystem::path &cache_file, const CacheKey &key);

// Whether `cache_file` is usable for `key`: the file exists, its sidecar
// records a key equal to `key`, and the file's size still matches the size
// recorded with it. A missing, corrupt, truncated or key-mismatched cache is
// stale - reported as false, never a crash.
bool cache_is_fresh(const std::filesystem::path &cache_file, const CacheKey &key);

} // namespace genesis::render
