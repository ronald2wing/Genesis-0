// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <filesystem>

#include "render/MediaCache.h"

namespace genesis::render {

// The reversed copy of a source span, and the key that records it. A reverse
// is per-span, not per-source: the same source reversed at two different
// in-points must not collide, so the span rides in the file's name - unlike
// the waveform/filmstrip/poster, whose names carry only the source. The name
// is the FNV-1a stem of the source path plus the two span doubles, so it is
// deterministic across runs; the span also rides in the recorded key, so a
// stale span reads stale.

// The reverse file for `source`'s `[start, start + duration)` span under
// `cache_root`. `duration` 0 means "to the end". Pure naming: no I/O, no
// codecs.
std::filesystem::path reverse_cache_path(const std::filesystem::path &cache_root,
                                         const std::filesystem::path &source, double start,
                                         double duration);

// The key that records what produced one reverse file, for cache_record /
// cache_is_fresh. `source_size` is the source's byte size at generation time.
CacheKey reverse_key(const std::filesystem::path &source, std::uintmax_t source_size, double start,
                     double duration);

} // namespace genesis::render
