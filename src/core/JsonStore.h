// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The one way a small JSON state file is read and written. Readers treat an
// absent, unreadable, oversized or corrupt file as "no state" (the caller
// decides what empty means); writers go through the crash-safe atomic
// replacement so no reader ever sees a half-written file and no crash window
// leaves the file missing.

#pragma once

#include <filesystem>
#include <optional>

#include <nlohmann/json.hpp>

namespace genesis::core {

// The parsed file, or nullopt when it is absent, unreadable, beyond the
// document bounds (see JsonGuard.h), or not valid JSON.
std::optional<nlohmann::json> read_json_file(const std::filesystem::path &path);

// Serialises `doc` (two-space indent, trailing newline) and atomically
// replaces `path` with it, creating the parent directory first. Returns false
// on any failure, leaving the previous file intact.
bool write_json_atomic(const std::filesystem::path &path, const nlohmann::json &doc);

} // namespace genesis::core
