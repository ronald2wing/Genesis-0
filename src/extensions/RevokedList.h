// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace genesis::extensions {

// One revoked id, as a marketplace catalog declares it (the catalog's
// `revoked` list) and as the store records it (`revoked.json`). Revocation is
// advisory data from a catalog the user chose to use: a signed catalog bounds
// the trust placed in its revocation list, and an unsigned catalog's list is
// only as trustworthy as the catalog itself. It is not a security boundary -
// it forces an installed extension off so the user can see why and remove it,
// rather than silently uninstalling.
//
// `version`, when present, revokes only that installed version (matched
// against the installed version directory name, as a string); absent revokes
// every version of the id. `reason` is the publisher's optional explanation,
// carried to the record the host surfaces.
struct Revocation
{
    std::string id;
    std::optional<std::string> version;
    std::optional<std::string> reason;

    bool operator==(const Revocation &) const = default;
};

// Whether `revocation` matches an installed `version`: a version-less
// revocation matches any version; a version-specific one matches only that
// version string, compared verbatim.
bool revocation_matches(const Revocation &revocation, const std::string &version);

// Reads the revocation list from `<store>/revoked.json`. An absent, unreadable
// or corrupt file is an empty list (nothing revoked), not an error.
std::vector<Revocation> read_revocations(const std::filesystem::path &store_root);

// The revocation matching `id` at installed `version`, or nullopt when the id
// is not revoked at that version.
std::optional<Revocation> revoked_at(const std::filesystem::path &store_root, const std::string &id,
                                     const std::string &version);

// Reconciles the store's list with `list`: the catalog is authoritative for
// its own revocations, so the store's list is replaced outright - an id the
// catalog no longer revokes is cleared. Returns false when the save fails
// (the in-memory state is unaffected, so the old list stays on disk).
bool replace_revocations(const std::filesystem::path &store_root,
                         const std::vector<Revocation> &list);

} // namespace genesis::extensions
