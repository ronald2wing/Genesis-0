// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <filesystem>

#include "render/MediaCache.h"

namespace genesis::render {

// The low-resolution stand-in for a source, and the key that records it. A
// proxy is per-source, not per-span: it is a whole-file downscale, so the
// source path (plus its size) names it - unlike a reverse, whose span rides in
// the name. The name is the FNV-1a stem of the source path, the same hash
// MediaCache::cache_paths uses for the poster/filmstrip/waveform, so a proxy
// lands beside its sibling caches under one stem.

// The proxy file for `source` under `cache_root`. Pure naming: no I/O, no
// codecs.
std::filesystem::path proxy_cache_path(const std::filesystem::path &cache_root,
                                       const std::filesystem::path &source);

// The key that records what produced one proxy file, for cache_record /
// cache_is_fresh. `source_size` is the source's byte size at generation time.
CacheKey proxy_key(const std::filesystem::path &source, std::uintmax_t source_size);

} // namespace genesis::render
