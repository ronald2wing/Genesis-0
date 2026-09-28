// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "project/DocumentFile.h"

#include <fstream>
#include <utility>

#include "core/AtomicWrite.h"
#include "core/JsonGuard.h"

namespace genesis::project {

std::expected<LoadedDocument, std::string> read_document_file(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected("could not read " + path.string());
    }
    std::string error;
    std::optional<nlohmann::json> parsed = core::parse_document_json(in, error);
    if (!parsed) {
        return std::unexpected(path.string() + ": " + error);
    }
    if (!parsed->is_object()) {
        return std::unexpected(path.string() + ": not a JSON object");
    }
    if (const std::optional<int> version = document_version(*parsed);
        version && *version > DOCUMENT_VERSION) {
        return std::unexpected(path.string() + " was saved by a newer build than this one"
                               + " (document version " + std::to_string(*version)
                               + "; this build reads up to " + std::to_string(DOCUMENT_VERSION)
                               + ")");
    }
    DocumentLoadReport report;
    std::optional<Project> loaded = from_document(*parsed, { }, &report);
    if (!loaded) {
        return std::unexpected(path.string() + " holds a document this build cannot read");
    }
    return LoadedDocument{ std::move(*loaded), std::move(*parsed), std::move(report.dropped_keys) };
}

std::optional<std::string> write_document_file(const std::filesystem::path &path,
                                               const nlohmann::json &doc)
{
    if (!core::write_file_atomic(path, doc.dump(2))) {
        return "could not write " + path.string();
    }
    return std::nullopt;
}

} // namespace genesis::project
