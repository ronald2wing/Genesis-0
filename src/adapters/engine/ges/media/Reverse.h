// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <span>

namespace genesis::render {
struct ExportClip;
}

namespace genesis::adapters::engine::ges {

// A request to reverse a span of a source, written to a file that plays it
// backwards. Decoding a source in reverse on the fly stalls a timeline; a
// cached file does not. The engine does the work (R5) - the host only names the
// source, the span and the destination.
struct ReverseRequest
{
    std::filesystem::path source;
    std::filesystem::path destination;
    double start = 0.0; // seconds into the source
    double duration = 0.0; // 0 = to the end
};

// Writes the reversed span and returns its path, or nullopt on failure.
// Blocking: callers keep it off the UI thread (R4).
std::optional<std::filesystem::path> make_reverse(const ReverseRequest &request);

// The reversed copy of `source`'s `[start, start + duration)` span under
// `cache_root`, generated if it is absent or stale. Returns the copy's path
// when a fresh one exists, and nullopt when there is nothing to reverse (an
// empty source or root, a missing source, a span that cannot be generated).
// Blocking, like `make_reverse`: keep it off the UI thread.
std::optional<std::filesystem::path> ensure_reverse(const std::filesystem::path &source,
                                                    double start, double duration,
                                                    const std::filesystem::path &cache_root);

// Ensures the reverse for every flattened clip that needs one: reversed video
// clips. Non-video and non-reversed clips are skipped; `cancelled` (when set)
// is polled between clips and abandons the rest. Blocking.
void ensure_reverses(std::span<const genesis::render::ExportClip> clips,
                     const std::filesystem::path &cache_root,
                     const std::function<bool()> &cancelled = { });

} // namespace genesis::adapters::engine::ges
