// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <filesystem>
#include <map>
#include <set>
#include <string>

namespace genesis::extensions {

// The record of which Builtin/Curated native extension ids the user has
// disabled. Developer extensions never consult it - their gate is consent
// (see Consent.h) - so it is the enable/disable switch for the origins that
// otherwise load freely.
//
// The list persists as `<store>/disabled.json`, in the shape
// `{"ids": [...], "reasons": {<id>: <reason>}}` where `reasons` maps an id to
// the sentence explaining why it is disabled (empty for a direct user
// disable). A file with only `{"ids": [...]}` (one written by hand) still
// reads, with every reason empty. An absent or unreadable file
// is an empty list (nothing is disabled), not an error; add/remove write the
// file back atomically. The class loads the list once at construction and
// keeps it in memory, so `disabled` is a cheap lookup the host does per
// extension.
class DisabledList
{
public:
    explicit DisabledList(std::filesystem::path store_root);

    // True when the id is on the disabled list.
    bool disabled(const std::string &id) const;

    // The recorded reason the id is disabled (empty for a direct user
    // disable, or when the id is not on the list).
    const std::string &reason(const std::string &id) const;

    // Adds the id to the list and persists it. `why` (empty by default) is the
    // recorded reason; an empty reason means a direct user disable. Returns
    // false when the save fails; the in-memory list is still updated, so the
    // disable takes effect for this process regardless.
    bool add(const std::string &id, const std::string &why = { });

    // Removes the id from the list and persists it. Returns false when the
    // save fails; the in-memory list is still updated.
    bool remove(const std::string &id);

    // The disabled ids, for inspection.
    const std::set<std::string> &ids() const { return disabled_; }

private:
    // Writes the list back to disk; returns false on any I/O failure.
    bool save() const;

    std::filesystem::path store_root_;
    std::set<std::string> disabled_;
    std::map<std::string, std::string> reasons_;
};

} // namespace genesis::extensions
