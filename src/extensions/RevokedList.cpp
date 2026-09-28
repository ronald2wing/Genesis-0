// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/RevokedList.h"

#include "core/JsonStore.h"

#include <nlohmann/json.hpp>

#include <system_error>
#include <utility>

namespace genesis::extensions {

namespace {

// The store-relative filename the revocation list persists under.
inline constexpr const char *kRevokedFile = "revoked.json";

// Loads the revocation list from disk. An absent, unreadable or corrupt file
// is an empty list, not an error; an entry that is not an object or that
// lacks a string `id` is skipped, the same leniency as the other store lists.
std::vector<Revocation> load(const std::filesystem::path &store_root)
{
    std::vector<Revocation> list;
    const std::optional<nlohmann::json> parsed = core::read_json_file(store_root / kRevokedFile);
    if (!parsed) {
        return list;
    }
    const nlohmann::json &doc = *parsed;
    if (!doc.is_object()) {
        return list;
    }
    const auto it = doc.find("revocations");
    if (it == doc.end() || !it->is_array()) {
        return list;
    }
    for (const nlohmann::json &entry : *it) {
        if (!entry.is_object()) {
            continue;
        }
        const auto id_it = entry.find("id");
        if (id_it == entry.end() || !id_it->is_string()) {
            continue;
        }
        Revocation revocation;
        revocation.id = id_it->get<std::string>();
        const auto version_it = entry.find("version");
        if (version_it != entry.end() && version_it->is_string()) {
            revocation.version = version_it->get<std::string>();
        }
        const auto reason_it = entry.find("reason");
        if (reason_it != entry.end() && reason_it->is_string()) {
            revocation.reason = reason_it->get<std::string>();
        }
        list.push_back(std::move(revocation));
    }
    return list;
}

// Writes the list back to disk; returns false on any I/O failure.
bool save(const std::filesystem::path &store_root, const std::vector<Revocation> &list)
{
    nlohmann::json revocations = nlohmann::json::array();
    for (const Revocation &revocation : list) {
        nlohmann::json entry = nlohmann::json::object();
        entry["id"] = revocation.id;
        if (revocation.version) {
            entry["version"] = *revocation.version;
        }
        if (revocation.reason) {
            entry["reason"] = *revocation.reason;
        }
        revocations.push_back(std::move(entry));
    }
    const nlohmann::json doc = nlohmann::json{ { "revocations", std::move(revocations) } };

    return core::write_json_atomic(store_root / kRevokedFile, doc);
}

} // namespace

bool revocation_matches(const Revocation &revocation, const std::string &version)
{
    return !revocation.version || *revocation.version == version;
}

std::vector<Revocation> read_revocations(const std::filesystem::path &store_root)
{
    return load(store_root);
}

std::optional<Revocation> revoked_at(const std::filesystem::path &store_root, const std::string &id,
                                     const std::string &version)
{
    for (const Revocation &revocation : load(store_root)) {
        if (revocation.id == id && revocation_matches(revocation, version)) {
            return revocation;
        }
    }
    return std::nullopt;
}

bool replace_revocations(const std::filesystem::path &store_root,
                         const std::vector<Revocation> &list)
{
    return save(store_root, list);
}

} // namespace genesis::extensions
