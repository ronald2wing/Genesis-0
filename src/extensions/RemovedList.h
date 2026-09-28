// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <filesystem>
#include <set>
#include <string>

namespace genesis::extensions {

// The tombstone of removed builtin extension ids. Removing a builtin writes its
// id here (so the startup seed never re-adds it silently) and deletes the
// installed copy; the record persists until the extension is restored or
// reinstalled. Store-installed (non-builtin) extensions are not tombstoned yet
// (Stage C).
//
// The list persists as `<store>/removed.json`, in the same `{"ids": [...]}`
// shape as the disabled and consent lists. An absent or unreadable file is an
// empty list (nothing removed), not an error; add/remove write the file back
// atomically. Each function reads the list fresh from disk, so the calls are
// independent and idempotent.

// True when `id` is on the removed list.
bool removed(const std::filesystem::path &store_root, const std::string &id);

// Tombstones `id` and persists the list. Returns false when the save fails; the
// removal still takes effect for this process regardless.
bool remove(const std::filesystem::path &store_root, const std::string &id);

// Clears the tombstone for `id` and persists the list. Returns false when the
// save fails.
bool restore(const std::filesystem::path &store_root, const std::string &id);

// The removed ids, for inspection.
std::set<std::string> ids(const std::filesystem::path &store_root);

} // namespace genesis::extensions
