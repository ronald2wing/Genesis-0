// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "workspace/CacheSweep.h"

#include <algorithm>
#include <string>
#include <system_error>

namespace genesis::workspace {

namespace {

// The sidecar path that records the key for a data file: its own path with
// ".key" appended, the same scheme render::MediaCache and MaskStore use.
std::filesystem::path sidecar_path(const std::filesystem::path &data_file)
{
    return std::filesystem::path(data_file.string() + ".key");
}

} // namespace

std::vector<std::filesystem::path> cache_roots(const std::filesystem::path &cache_root)
{
    if (cache_root.empty()) {
        return { };
    }
    return { cache_root, cache_root / "masks", cache_root / "titles" };
}

std::vector<CacheEntry> cache_entries(const std::vector<std::filesystem::path> &roots)
{
    std::vector<CacheEntry> out;
    for (const std::filesystem::path &root : roots) {
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) {
            continue;
        }
        std::filesystem::directory_iterator it(root, ec);
        const std::filesystem::directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            const std::filesystem::directory_entry &entry = *it;
            std::error_code type_ec;
            if (!entry.is_regular_file(type_ec)) {
                continue;
            }
            const std::filesystem::path &path = entry.path();
            const std::string name = path.filename().string();
            if (name.ends_with(".key") || name.ends_with(".tmp")) {
                continue;
            }
            std::error_code size_ec;
            const std::uintmax_t size = entry.file_size(size_ec);
            if (size_ec) {
                continue;
            }
            CacheEntry cache;
            cache.path = path;
            cache.size = size;
            cache.mtime = entry.last_write_time(size_ec);
            out.push_back(std::move(cache));
        }
    }
    return out;
}

std::uintmax_t cache_size_bytes(const std::vector<CacheEntry> &entries)
{
    std::uintmax_t total = 0;
    for (const CacheEntry &entry : entries) {
        total += entry.size;
    }
    return total;
}

SweepResult sweep(const std::vector<std::filesystem::path> &roots, std::uintmax_t cap_bytes)
{
    SweepResult result;
    std::vector<CacheEntry> entries = cache_entries(roots);
    std::uintmax_t total = cache_size_bytes(entries);
    if (total <= cap_bytes) {
        result.bytes_remaining = total;
        return result;
    }

    // Oldest first: the least recently written cache is evicted before a
    // recently used one, so a hot cache survives a trim.
    std::sort(entries.begin(), entries.end(),
              [](const CacheEntry &a, const CacheEntry &b) { return a.mtime < b.mtime; });
    for (const CacheEntry &entry : entries) {
        if (total <= cap_bytes) {
            break;
        }
        std::error_code ec;
        std::filesystem::remove(entry.path, ec);
        if (ec) {
            continue; // an unremovable entry is skipped; the rest still proceed
        }
        total -= entry.size;
        ++result.files_evicted;
        result.bytes_evicted += entry.size;
        std::error_code ignored;
        std::filesystem::remove(sidecar_path(entry.path), ignored);
    }
    result.bytes_remaining = total;
    return result;
}

std::size_t clear_cache(const std::vector<std::filesystem::path> &roots)
{
    std::size_t removed = 0;
    for (const CacheEntry &entry : cache_entries(roots)) {
        std::error_code ec;
        std::filesystem::remove(entry.path, ec);
        if (ec) {
            continue;
        }
        ++removed;
        std::error_code ignored;
        std::filesystem::remove(sidecar_path(entry.path), ignored);
    }
    return removed;
}

} // namespace genesis::workspace
