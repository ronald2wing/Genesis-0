// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/RevokedList.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <system_error>
#include <utility>

namespace genesis::extensions {

namespace {

// The store-relative filename the revocation list persists under.
inline constexpr const char *kRevokedFile = "revoked.json";

// A staging name used only while the list is being rewritten, so a reader
// never sees a half-written list.
inline constexpr const char *kStagingFile = ".tmp-revoked.json";

// Loads the revocation list from disk. An absent, unreadable or corrupt file
// is an empty list, not an error; an entry that is not an object or that
// lacks a string `id` is skipped, the same leniency as the other store lists.
std::vector<Revocation> load(const std::filesystem::path &store_root)
{
    std::vector<Revocation> list;
    std::ifstream in(store_root / kRevokedFile);
    if (!in) {
        return list;
    }
    nlohmann::json doc;
    try {
        in >> doc;
    } catch (const nlohmann::json::exception &) {
        return list;
    }
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

    std::error_code ec;
    std::filesystem::create_directories(store_root, ec);
    if (ec) {
        return false;
    }

    // Write beside the destination (same filesystem) and rename, so the list
    // is never observed half-written.
    const std::filesystem::path staging = store_root / kStagingFile;
    const std::filesystem::path final = store_root / kRevokedFile;

    std::ofstream out(staging, std::ios::trunc);
    if (!out) {
        return false;
    }
    out << doc.dump(2) << '\n';
    out.close();
    if (!out) {
        std::error_code ignored;
        std::filesystem::remove(staging, ignored);
        return false;
    }

    std::filesystem::remove(final, ec);
    ec.clear();
    std::filesystem::rename(staging, final, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(staging, ignored);
        return false;
    }
    return true;
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
