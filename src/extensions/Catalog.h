// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "extensions/Install.h"
#include "extensions/RevokedList.h"
#include "workspace/update/Signature.h"

namespace genesis::extensions {

// A marketplace catalog is a versioned JSON document listing installable
// extensions, each with the source that would reproduce it. It is the
// counterpart to a config profile (Config.h): a profile is the machine's own
// installed set, a catalog is a publisher's offer. Install trusts neither one
// more than the other - the same never-elevate-trust origin rules apply when an
// entry is installed.
//
// The catalog document is `{entries: [...], revoked?: [...]}`; each entry is
// described beside `CatalogEntry` and each revocation beside `Revocation`.
// There is exactly one catalog format, so no version number is carried or
// read; an unknown key is ignored rather than refused.

// One entry in a catalog: the identity a caller picks from, plus the source
// that would install it. `version` is a string carried verbatim from the
// document (a publisher may spell it how it likes); the install itself reads
// the version from the installed manifest, never from this field.
//
// `sha256`, when present, is the content digest the source must match before
// install (see SourceInstall.h `tree_sha256` for the per-type convention); the
// install path verifies it. `signature` is the publisher's Ed25519 signature
// over `signed_entry_payload(entry)` (the id and the digest), verified against
// kCatalogPublicKey with the fail-closed rules below - a signed entry with no
// verifier, no key, or a bad signature is refused.
struct CatalogEntry
{
    std::string id;
    std::string name;
    std::string version;
    std::string description;
    InstallSource source;
    std::optional<std::string> sha256;
    std::optional<std::string> signature;
};

// The embedded catalog signing key. Until a catalog signing key is minted this
// is all zeros, which verifies nothing - a signed entry is refused. Replace
// these bytes with the real public half before catalog signatures can verify;
// which key signs catalog entries and where the public half ships is pending
// (the release key in workspace/update/Signature.h is a different trust
// domain and is not reused). Mirrors kReleasePublicKey.
inline constexpr workspace::update::PublicKey kCatalogPublicKey{ };

// Whether a real catalog signing key is embedded (any non-zero byte).
bool catalog_key_configured();

// The canonical bytes a catalog entry's signature covers: the id, a newline,
// then the sha256 digest. The signature binds the identity to the content, and
// covers only the digest so it verifies before the source is fetched - the
// digest check then pins the actual bytes (SourceInstall.h). Mirrors
// workspace::update::Manifest::signed_payload.
std::string signed_entry_payload(const CatalogEntry &entry);

// Verifies the entry's signature over signed_entry_payload against `key`.
// `Rejected` for a missing/malformed signature, a signature without a sha256 to
// bind to, or a signature that does not verify; `Unavailable` when crypto is
// not compiled in (GENESIS_UPDATER off).
workspace::update::VerifyResult verify_entry_signature(const CatalogEntry &entry,
                                                       const workspace::update::PublicKey &key);

// What reading a catalog produced. `entries` is set only when the document
// parses and validates completely; any problem leaves it empty, so a caller
// can never half-consume a broken catalog. `revocations` carries the document's
// `revoked` list (empty when the document omits it or does not parse).
// `problems` is empty on success.
struct CatalogResult
{
    std::optional<std::vector<CatalogEntry>> entries;
    std::vector<Revocation> revocations;
    std::vector<std::string> problems;
};

// Parses and validates a catalog document. Refused when the document is not an
// object, `entries` is not an array, or an entry is malformed (a
// missing/blank/unnamespaced id, a missing name, or a source that is not a
// `dir`/`git` with a non-empty url). The optional `revoked` array is read the
// same all-or-nothing way: a malformed revocation (a missing/blank/unnamespaced
// id, or a non-string `version`/`reason`) fails the whole catalog.
CatalogResult parse_catalog(std::string_view text);

// A URL -> body fetch, injected so the host stays free of any HTTP client. An
// empty function means "no fetcher wired"; a returned nullopt means the fetch
// itself failed (the caller's error text).
using CatalogFetcher = std::function<std::optional<std::string>(const std::string &)>;

// Fetches and parses a catalog. Returns the entries, or (with `entries`
// empty) the reasons: no fetcher wired, the fetch failed, or the document did
// not parse/validate.
CatalogResult fetch_catalog(const std::string &url, const CatalogFetcher &fetcher);

// Fetches a catalog and installs the entry named `id`, following the entry's
// source through install_from_source (so a `git` entry clones, a `dir` entry
// installs the directory). The origin rules are install_from_source's: this
// never elevates trust. The entry's integrity fields are enforced first: a
// `signature` is verified fail-closed (see verify_entry_signature), and a
// `sha256` is passed to install_from_source to digest-verify the source before
// anything is copied. An entry the catalog's own `revoked` list names is
// refused before any source work, and the catalog's revocation list is
// reconciled into the store's `revoked.json` so a later scan enforces it.
// Returns the installed root, or the reasons (no fetcher, fetch/parse failure,
// unknown id, a refused integrity check, a revoked entry, or a refused
// install).
InstallResult install_from_catalog(const std::string &url, const std::string &id,
                                   const std::filesystem::path &store_root, Origin origin,
                                   const CatalogFetcher &fetcher);

// The entries as the JSON array a caller lists (the API's `extensions.catalog`
// reply): each entry's id, name, version, description, source, and (when
// present) its sha256 and signature.
nlohmann::json catalog_entries_json(const std::vector<CatalogEntry> &entries);

} // namespace genesis::extensions
