// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace genesis::ai {

// One model's manifest: where it comes from, how it is verified, and under what
// licence its weights may be used. The shape mirrors the host-owned download
// row the AI provider decision pins down (docs/decisions/ai-provider.md §5, §6):
// every model carries a SHA-256, and a download with nothing to check against
// is refused. The runtime that runs a model is a separate question from the
// model's own licence; only the latter is recorded here.
struct ModelEntry
{
    // Stable slug the store keys its directory on (e.g. "whisper").
    std::string id;
    // Version or variant, a path segment (<root>/<id>/<version>/).
    std::string version;
    // The upstream download URL, pinned to an immutable revision. Empty when
    // the model is a candidate that no provider has pinned yet.
    std::string url;
    // An optional mirror, tried before `url`. Empty when there is none.
    std::string mirror;
    // The SHA-256 of the downloaded bytes, 64 lowercase hex digits. Empty when
    // unpinned - the store refuses any such entry outright (ai-provider.md §6).
    std::string sha256;
    // The weights' SPDX licence identifier (e.g. "GPL-3.0"). It travels with
    // the download, beside the digest, so the licence is never separated from
    // the bytes it governs.
    std::string license;
    // Expected download size in bytes; 0 = unknown (unpinned).
    std::uintmax_t size = 0;
};

// The embedded default catalogue: the models Genesis-0 knows how to obtain and
// run. It is a data table, not logic. Rows whose provider is not selected yet
// keep their `url`, `mirror`, `sha256` and `size` unset - they are named
// candidates, and the store refuses to download them until a provider pins
// their provenance (docs/decisions/ai-provider.md §4.5 "No model is chosen
// here", §6). No runtime is implied by a row's presence.
std::span<const ModelEntry> model_catalogue();

} // namespace genesis::ai
