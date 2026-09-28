// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/RemovedList.h"

#include "core/JsonStore.h"
#include "core/TomlRead.h"

#include <nlohmann/json.hpp>

#include <system_error>
#include <utility>

namespace genesis::extensions {

namespace {

// The store-relative filename the removed list persists under.
inline constexpr const char *kRemovedFile = "removed.json";

// Loads the removed list from disk. An absent, unreadable or corrupt file is an
// empty list, not an error.
std::set<std::string> load(const std::filesystem::path &store_root)
{
    std::set<std::string> list;
    const std::optional<nlohmann::json> parsed = core::read_json_file(store_root / kRemovedFile);
    if (!parsed) {
        return list;
    }
    const nlohmann::json &doc = *parsed;
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

    return core::write_json_atomic(store_root / kRemovedFile, doc);
}

} // namespace

bool removed(const std::filesystem::path &store_root, const std::string &id)
{
    const std::set<std::string> list = load(store_root);
    return list.find(id) != list.end();
}

bool remove(const std::filesystem::path &store_root, const std::string &id)
{
    if (!core::is_namespaced_id(id)) {
        return false;
    }
    std::set<std::string> list = load(store_root);
    list.insert(id);
    return save(store_root, list);
}

bool restore(const std::filesystem::path &store_root, const std::string &id)
{
    if (!core::is_namespaced_id(id)) {
        return false;
    }
    std::set<std::string> list = load(store_root);
    list.erase(id);
    return save(store_root, list);
}

std::set<std::string> ids(const std::filesystem::path &store_root)
{
    return load(store_root);
}

} // namespace genesis::extensions
