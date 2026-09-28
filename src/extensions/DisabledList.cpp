// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/DisabledList.h"

#include "core/JsonStore.h"

#include <nlohmann/json.hpp>

#include <system_error>
#include <utility>

namespace genesis::extensions {

namespace {

// The store-relative filename the disabled list persists under.
inline constexpr const char *kDisabledFile = "disabled.json";

} // namespace

DisabledList::DisabledList(std::filesystem::path store_root) : store_root_(std::move(store_root))
{
    const std::optional<nlohmann::json> parsed = core::read_json_file(store_root_ / kDisabledFile);
    if (!parsed) {
        return; // absent or unreadable: an empty list, not an error
    }
    const nlohmann::json &doc = *parsed;
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
    // The reasons object is optional: a list written by hand carries only
    // `ids`, and every reason then reads empty.
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

    return core::write_json_atomic(store_root_ / kDisabledFile, doc);
}

} // namespace genesis::extensions
