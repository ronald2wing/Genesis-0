// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "render/ProxyCache.h"

#include <cstdint>

namespace genesis::render {

// The proxy is named by the source path's FNV-1a stem - the same stem the
// poster/filmstrip/waveform use (MediaCache::cache_stem_for_path), so a path
// maps to the same stem every run and only the extension differs, keeping the
// proxy beside its sibling caches.
std::filesystem::path proxy_cache_path(const std::filesystem::path &cache_root,
                                       const std::filesystem::path &source)
{
    return cache_root / (cache_stem_for_path(source) + ".proxy.webm");
}

CacheKey proxy_key(const std::filesystem::path &source, std::uintmax_t source_size)
{
    CacheKey key;
    key.source = source;
    key.source_size = source_size;
    return key;
}

} // namespace genesis::render
