// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/RemovedList.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <system_error>
#include <utility>

namespace genesis::extensions {

namespace {

// The store-relative filename the removed list persists under.
inline constexpr const char *kRemovedFile = "removed.json";

// A staging name used only while the list is being rewritten, so a reader
// never sees a half-written list.
inline constexpr const char *kStagingFile = ".tmp-removed.json";

// Loads the removed list from disk. An absent, unreadable or corrupt file is an
// empty list, not an error.
std::set<std::string> load(const std::filesystem::path &store_root)
{
    std::set<std::string> list;
    std::ifstream in(store_root / kRemovedFile);
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
    const auto it = doc.find("ids");
    if (it == doc.end() || !it->is_array()) {
        return list;
    }
    for (const nlohmann::json &id : *it) {
        if (id.is_string()) {
            list.insert(id.get<std::string>());
        }
    }
    return list;
}

// Writes the list back to disk; returns false on any I/O failure.
bool save(const std::filesystem::path &store_root, const std::set<std::string> &list)
{
    nlohmann::json ids = nlohmann::json::array();
    for (const std::string &id : list) {
        ids.push_back(id);
    }
    const nlohmann::json doc = nlohmann::json{ { "ids", std::move(ids) } };

    std::error_code ec;
    std::filesystem::create_directories(store_root, ec);
    if (ec) {
        return false;
    }

    // Write beside the destination (same filesystem) and rename, so the list
    // is never observed half-written.
    const std::filesystem::path staging = store_root / kStagingFile;
    const std::filesystem::path final = store_root / kRemovedFile;

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

bool removed(const std::filesystem::path &store_root, const std::string &id)
{
    const std::set<std::string> list = load(store_root);
    return list.find(id) != list.end();
}

bool remove(const std::filesystem::path &store_root, const std::string &id)
{
    std::set<std::string> list = load(store_root);
    list.insert(id);
    return save(store_root, list);
}

bool restore(const std::filesystem::path &store_root, const std::string &id)
{
    std::set<std::string> list = load(store_root);
    list.erase(id);
    return save(store_root, list);
}

std::set<std::string> ids(const std::filesystem::path &store_root)
{
    return load(store_root);
}

} // namespace genesis::extensions
