// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Reading and writing project document FILES: the bounded parse, the version
// gate and the atomic replacement live here once, shared by the API, the CLI
// and the app. Document.h stays the in-memory contract (json <-> Project);
// this header owns the filesystem edge around it.

#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "project/Document.h"
#include "project/Project.h"

namespace genesis::project {

// A document file the reader accepted: the settled project, the raw document
// it was built from, and the top-level keys the reader dropped (a save would
// rewrite them away, so callers may want to warn).
struct LoadedDocument
{
    Project project;
    nlohmann::json document;
    std::vector<std::string> dropped_keys;
};

// Reads, bounds, parses, version-gates and settles the document at `path`.
// The error is one legible sentence naming the path: unreadable, over the
// document bounds, not JSON, written by a newer build, or not a document this
// build can read.
std::expected<LoadedDocument, std::string> read_document_file(const std::filesystem::path &path);

// Serialises `doc` (two-space indent) and atomically replaces `path` with it,
// so a crash mid-save never leaves a truncated project on disk. Returns
// nullopt on success, or why it failed.
std::optional<std::string> write_document_file(const std::filesystem::path &path,
                                               const nlohmann::json &doc);

} // namespace genesis::project
