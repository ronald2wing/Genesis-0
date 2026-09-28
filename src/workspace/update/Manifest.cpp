// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "workspace/update/Manifest.h"

#include <string>

#include <nlohmann/json.hpp>

#include "core/JsonGuard.h"

namespace genesis::workspace::update {

namespace core = genesis::core;

std::string signed_payload(const Manifest &manifest)
{
    return manifest.version + "\n" + manifest.url + "\n" + manifest.sha256 + "\n";
}

namespace {

bool is_lower_hex(std::string_view text)
{
    for (const char c : text) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) {
            return false;
        }
    }
    return true;
}

// A required non-empty string field, or nullopt when absent or the wrong type.
std::optional<std::string> required_string(const nlohmann::json &doc, const char *field)
{
    if (!doc.contains(field) || !doc[field].is_string()) {
        return std::nullopt;
    }
    const std::string value = doc[field].get<std::string>();
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

} // namespace

ParseResult parse_manifest(std::string_view json)
{
    ParseResult result;

    // Refuse an oversized or over-nested manifest on its raw bytes, before the
    // recursive parser runs.
    if (const std::optional<std::string> problem = core::document_json_guard(json)) {
        result.problems.push_back("the manifest " + *problem);
        return result;
    }

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(json);
    } catch (const nlohmann::json::exception &error) {
        result.problems.push_back(std::string("the manifest is not valid JSON: ") + error.what());
        return result;
    }
    if (!doc.is_object()) {
        result.problems.push_back("the manifest is not a JSON object");
        return result;
    }

    Manifest manifest;
    const std::optional<std::string> version = required_string(doc, "version");
    const std::optional<std::string> url = required_string(doc, "url");
    const std::optional<std::string> sha256 = required_string(doc, "sha256");
    const std::optional<std::string> signature = required_string(doc, "signature");

    if (!version) {
        result.problems.push_back("the manifest has no version");
    }
    if (!url) {
        result.problems.push_back("the manifest has no url");
    }
    if (!sha256) {
        result.problems.push_back("the manifest has no sha256");
    } else if (sha256->size() != 64 || !is_lower_hex(*sha256)) {
        result.problems.push_back("the manifest sha256 is not 64 lowercase hex digits");
    }
    if (!signature) {
        result.problems.push_back("the manifest has no signature");
    } else if (signature->size() != 128 || !is_lower_hex(*signature)) {
        result.problems.push_back("the manifest signature is not 128 lowercase hex digits");
    }

    if (!result.problems.empty()) {
        return result;
    }

    manifest.version = *version;
    manifest.url = *url;
    manifest.sha256 = *sha256;
    manifest.signature = *signature;
    // Notes are optional and unsigned (see Manifest.h): read tolerantly so a
    // manifest without them still parses, and a malformed notes field is
    // ignored rather than refusing the whole manifest.
    if (doc.contains("notes") && doc["notes"].is_string()) {
        manifest.notes = doc["notes"].get<std::string>();
    }
    result.manifest = std::move(manifest);
    return result;
}

} // namespace genesis::workspace::update
