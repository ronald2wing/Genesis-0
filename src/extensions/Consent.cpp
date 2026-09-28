// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/Consent.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <system_error>
#include <utility>

namespace genesis::extensions {

namespace {

// The store-relative filename the consent list persists under.
inline constexpr const char *kConsentFile = "developer-consent.json";

// A staging name used only while the consent file is being rewritten, so a
// reader never sees a half-written list.
inline constexpr const char *kStagingFile = ".tmp-developer-consent.json";

} // namespace

Consent::Consent(std::filesystem::path store_root) : store_root_(std::move(store_root))
{
    std::ifstream in(store_root_ / kConsentFile);
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

    std::error_code ec;
    std::filesystem::create_directories(store_root_, ec);
    if (ec) {
        return false;
    }

    // Write beside the destination (same filesystem) and rename, so the list is
    // never observed half-written.
    const std::filesystem::path staging = store_root_ / kStagingFile;
    const std::filesystem::path final = store_root_ / kConsentFile;

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
