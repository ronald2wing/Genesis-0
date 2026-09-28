// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <filesystem>
#include <set>
#include <string>

namespace genesis::extensions {

// The record of which Developer-origin native extension ids the user has
// consented to load. Consent is the gate that turns an unsigned, local-only
// native extension from "visible but not loadable" into loadable (see
// Trust.h's may_load_native). Builtin and Curated extensions never consult it:
// they load freely.
//
// The list persists as `<store>/developer-consent.json`. An absent or
// unreadable file is an empty list (nothing is consented), not an error; a
// grant/revoke writes the file back atomically. The class loads the list once
// at construction and keeps it in memory, so `consented` is a cheap lookup the
// loader can do per extension.
class Consent
{
public:
    explicit Consent(std::filesystem::path store_root);

    // True when the id is on the consent list.
    bool consented(const std::string &id) const;

    // Adds the id to the list and persists it. Returns false when the save
    // fails; the in-memory list is still updated, so the consent takes effect
    // for this process regardless.
    bool grant(const std::string &id);

    // Removes the id from the list and persists it. Returns false when the
    // save fails; the in-memory list is still updated.
    bool revoke(const std::string &id);

    // The consented ids, for inspection.
    const std::set<std::string> &ids() const { return consented_; }

private:
    // Writes the list back to disk; returns false on any I/O failure.
    bool save() const;

    std::filesystem::path store_root_;
    std::set<std::string> consented_;
};

} // namespace genesis::extensions
