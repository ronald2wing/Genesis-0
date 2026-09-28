// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ai/ModelManifest.h"

namespace genesis::ai {

// Where an installed model lives: <root>/<id>/<version>/<filename>, `filename`
// being the basename of the entry's upstream URL. Two versions of one id sit
// side by side, exactly like the Extension install store (extensions/Install).
//
// The caller-supplied root is not where the app keeps evictable caches; models
// belong under the app-data location (AppDataLocation/models), never the cache
// root.
std::filesystem::path model_path(const std::filesystem::path &root, const ModelEntry &entry);

// Whether a verified copy of `entry` is installed under `root`: the model file
// exists and its recomputed SHA-256 matches entry.sha256. A corrupt, truncated
// or replaced file reads false, never a crash. An entry with no digest can
// never be "installed" - there is nothing to check against.
bool installed(const std::filesystem::path &root, const ModelEntry &entry);

// A transport for one model. It is handed the entry (for its URLs) and a sink
// it must feed every downloaded byte through, chunk by chunk. The store owns
// the partial file, the streaming digest, progress accounting and the atomic
// rename; the fetcher owns only transport. A real implementation streams an
// HTTP body (mirror before upstream, per docs/decisions/ai-provider.md §3(c));
// tests inject a fake that feeds canned bytes. `sink` returns false when the
// store wants the transfer stopped; a fetcher that sees that must stop and
// return false itself.
using Fetcher =
        std::function<bool(const ModelEntry &entry,
                           const std::function<bool(std::span<const std::uint8_t> chunk)> &sink)>;

// Reports download progress: bytes written so far, and the entry's expected
// total (0 when the entry does not pin a size).
using Progress = std::function<void(std::uintmax_t bytes, std::uintmax_t total)>;

struct InstallResult
{
    std::optional<std::filesystem::path> installed;
    std::vector<std::string> problems;
    bool ok() const { return installed.has_value(); }
};

// Downloads `entry` into `root` and returns the verified model path, or the
// reasons it was refused. The flow, in order:
//
//   1. Refuse an entry with no sha256 (or a malformed one), no id/version, or
//      no URL to fetch - BEFORE any file is created.
//   2. If a verified copy is already installed, reuse it without fetching.
//   3. Stream the bytes through `fetcher` into a `.tmp-*` partial beside the
//      destination, computing SHA-256 as they arrive.
//   4. On digest match: atomically rename into place and write the
//      licence/digest sidecar (`.meta`). On mismatch or an interrupted fetch:
//      delete the partial and fail, leaving nothing behind.
InstallResult install(const std::filesystem::path &root, const ModelEntry &entry,
                      const Fetcher &fetcher, const Progress &progress = { });

} // namespace genesis::ai
