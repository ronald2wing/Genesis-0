// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/DisabledList.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <system_error>
#include <utility>

namespace genesis::extensions {

namespace {

// The store-relative filename the disabled list persists under.
inline constexpr const char *kDisabledFile = "disabled.json";

// A staging name used only while the list is being rewritten, so a reader
// never sees a half-written list.
inline constexpr const char *kStagingFile = ".tmp-disabled.json";

} // namespace

DisabledList::DisabledList(std::filesystem::path store_root) : store_root_(std::move(store_root))
{
    std::ifstream in(store_root_ / kDisabledFile);
    if (!in) {
        return; // absent or unreadable: an empty list, not an error
    }
    nlohmann::json doc;
    try {
        in >> doc;
    } catch (const nlohmann::json::exception &) {
        return; // a corrupt list is treated as empty
    }
    if (!doc.is_object()) {
        return;
    }
    const auto it = doc.find("ids");
    if (it != doc.end() && it->is_array()) {
        for (const nlohmann::json &id : *it) {
            if (id.is_string()) {
                disabled_.insert(id.get<std::string>());
            }
        }
    }
    // The reasons object is optional: an older list (or one written by hand)
    // carries only `ids`, and every reason then reads empty.
    const auto reasons = doc.find("reasons");
    if (reasons != doc.end() && reasons->is_object()) {
        for (const auto &[id, why] : reasons->items()) {
            if (why.is_string()) {
                reasons_[id] = why.get<std::string>();
            }
        }
    }
}

bool DisabledList::disabled(const std::string &id) const
{
    return disabled_.find(id) != disabled_.end();
}

const std::string &DisabledList::reason(const std::string &id) const
{
    static const std::string empty;
    const auto it = reasons_.find(id);
    return it == reasons_.end() ? empty : it->second;
}

bool DisabledList::add(const std::string &id, const std::string &why)
{
    disabled_.insert(id);
    if (why.empty()) {
        reasons_.erase(id);
    } else {
        reasons_[id] = why;
    }
    return save();
}

bool DisabledList::remove(const std::string &id)
{
    disabled_.erase(id);
    reasons_.erase(id);
    return save();
}

bool DisabledList::save() const
{
    nlohmann::json ids = nlohmann::json::array();
    for (const std::string &id : disabled_) {
        ids.push_back(id);
    }
    nlohmann::json reasons = nlohmann::json::object();
    for (const auto &[id, why] : reasons_) {
        reasons[id] = why;
    }
    const nlohmann::json doc =
            nlohmann::json{ { "ids", std::move(ids) }, { "reasons", std::move(reasons) } };

    std::error_code ec;
    std::filesystem::create_directories(store_root_, ec);
    if (ec) {
        return false;
    }

    // Write beside the destination (same filesystem) and rename, so the list
    // is never observed half-written.
    const std::filesystem::path staging = store_root_ / kStagingFile;
    const std::filesystem::path final = store_root_ / kDisabledFile;

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

} // namespace genesis::extensions
