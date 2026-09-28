// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/Consent.h"

#include "core/JsonStore.h"

#include <nlohmann/json.hpp>

#include <system_error>
#include <utility>

namespace genesis::extensions {

namespace {

// The store-relative filename the consent list persists under.
inline constexpr const char *kConsentFile = "developer-consent.json";

} // namespace

Consent::Consent(std::filesystem::path store_root) : store_root_(std::move(store_root))
{
    const std::optional<nlohmann::json> parsed = core::read_json_file(store_root_ / kConsentFile);
    if (!parsed) {
        return; // absent or unreadable: an empty list, not an error
    }
    const nlohmann::json &doc = *parsed;
    if (!doc.is_object()) {
        return;
    }
    const auto it = doc.find("ids");
    if (it == doc.end() || !it->is_array()) {
        return;
    }
    for (const nlohmann::json &id : *it) {
        if (id.is_string()) {
            consented_.insert(id.get<std::string>());
        }
    }
}

bool Consent::consented(const std::string &id) const
{
    return consented_.find(id) != consented_.end();
}

bool Consent::grant(const std::string &id)
{
    consented_.insert(id);
    return save();
}

bool Consent::revoke(const std::string &id)
{
    consented_.erase(id);
    return save();
}

bool Consent::save() const
{
    nlohmann::json ids = nlohmann::json::array();
    for (const std::string &id : consented_) {
        ids.push_back(id);
    }
    const nlohmann::json doc = nlohmann::json{ { "ids", std::move(ids) } };

    return core::write_json_atomic(store_root_ / kConsentFile, doc);
}

} // namespace genesis::extensions
