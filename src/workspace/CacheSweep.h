// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The processed-media cache sweep: the host-side helper that bounds the
// caches the app writes (waveforms, filmstrips, posters, reverses, proxies,
// masks, title PNGs) to a configurable cap. It only ever enumerates the three
// cache roots and deletes the files it finds there, so source assets and
// extension packs are never touched. Budgeted by disk usage (a cap plus
// oldest-first eviction), not by physical RAM — see
// `docs/decisions/cache-and-memory-budgeting.md`.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace genesis::workspace {

// The default processed-media cache cap: 10 GiB. The persisted value lives in
// AppSettings (`cache.json` in the config dir); this constant is the fallback a
// fresh install starts from and the number the Settings -> Storage section
// seeds its field with.
inline constexpr std::uintmax_t kDefaultCacheCapBytes = 10ULL * 1024 * 1024 * 1024;

// The directories the processed-media caches live under, relative to the cache
// root: the flat root (waveform/filmstrip/poster/reverse/proxy files) plus the
// two subdirectories (masks, titles). Sweeping these three - and nothing else -
// is the whole policy.
std::vector<std::filesystem::path> cache_roots(const std::filesystem::path &cache_root);

// One cache data file: its path, its size and its last-write time (the eviction
// key). The `.key` sidecars are not entries - they are removed alongside their
// data file when it is evicted, and an orphaned one (from a crashed write) is
// harmless: cache_is_fresh rejects a missing data file.
struct CacheEntry
{
    std::filesystem::path path;
    std::uintmax_t size = 0;
    std::filesystem::file_time_type mtime{ };
};

// Every cache data file under `roots`, in no particular order. Skips `.key`
// sidecars and `.tmp` staging files; a missing root contributes nothing.
std::vector<CacheEntry> cache_entries(const std::vector<std::filesystem::path> &roots);

// The summed size of `entries` in bytes.
std::uintmax_t cache_size_bytes(const std::vector<CacheEntry> &entries);

struct SweepResult
{
    std::size_t files_evicted = 0;
    std::uintmax_t bytes_evicted = 0;
    std::uintmax_t bytes_remaining = 0;
};

// Evicts the oldest entries until the total fits `cap_bytes`. Each eviction is
// one atomic std::filesystem::remove of the data file (then its sidecar); the
// mtime order means the least recently written file goes first. Never touches
// anything outside `roots`.
SweepResult sweep(const std::vector<std::filesystem::path> &roots, std::uintmax_t cap_bytes);

// Removes every cache data file (and its sidecar) under `roots`, returning the
// number of files removed.
std::size_t clear_cache(const std::vector<std::filesystem::path> &roots);

} // namespace genesis::workspace
