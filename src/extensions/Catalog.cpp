// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/Catalog.h"

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "core/TomlRead.h"
#include "extensions/SourceInstall.h"
#include "extensions/Sources.h"

namespace genesis::extensions {

namespace {

// Accumulates the entries and the problems found while reading the document. A
// problem recorded anywhere leaves the whole catalog unusable; the reader keeps
// going so a human sees every problem at once rather than only the first.
struct Builder
{
    std::vector<CatalogEntry> entries;
    std::vector<Revocation> revocations;
    std::vector<std::string> problems;

    void fail(std::string message) { problems.push_back(std::move(message)); }
};

// Reads one entry's `source` object: a `dir` or `git` type with a non-empty
// `url` (and, for `git`, optional `ref`/`subdir`). Returns false when the shape
// is unusable, leaving `out` untouched.
bool read_source(const nlohmann::json &entry, InstallSource &out, Builder &builder)
{
    const auto it = entry.find("source");
    if (it == entry.end() || !it->is_object()) {
        builder.fail("an entry has no source object");
        return false;
    }
    const nlohmann::json &source = *it;
    const auto type_it = source.find("type");
    if (type_it == source.end() || !type_it->is_string()) {
        builder.fail("a source has no `type`");
        return false;
    }
    const std::optional<SourceType> type = parse_source_type(type_it->get<std::string>());
    if (!type || (*type != SourceType::Dir && *type != SourceType::Git)) {
        builder.fail("a source `type` must be `dir` or `git`");
        return false;
    }
    const auto url_it = source.find("url");
    if (url_it == source.end() || !url_it->is_string() || url_it->get<std::string>().empty()) {
        builder.fail("a source must have a non-empty `url`");
        return false;
    }
    out.type = *type;
    out.url = url_it->get<std::string>();
    const auto ref_it = source.find("ref");
    if (ref_it != source.end() && ref_it->is_string()) {
        out.ref = ref_it->get<std::string>();
    }
    const auto subdir_it = source.find("subdir");
    if (subdir_it != source.end() && subdir_it->is_string()) {
        out.subdir = subdir_it->get<std::string>();
    }
    return true;
}

// Reads one entry. Returns false when it is unusable (a problem is recorded);
// on success the entry is appended.
void read_entry(const nlohmann::json &entry, Builder &builder)
{
    if (!entry.is_object()) {
        builder.fail("an entry is not an object");
        return;
    }
    const auto id_it = entry.find("id");
    if (id_it == entry.end() || !id_it->is_string()
        || !core::is_namespaced_id(id_it->get<std::string>())) {
        builder.fail("an entry's id is missing or not a namespaced id");
        return;
    }
    const auto name_it = entry.find("name");
    if (name_it == entry.end() || !name_it->is_string() || name_it->get<std::string>().empty()) {
        builder.fail("an entry's name is missing or empty");
        return;
    }

    CatalogEntry parsed;
    parsed.id = id_it->get<std::string>();
    parsed.name = name_it->get<std::string>();
    const auto version_it = entry.find("version");
    if (version_it != entry.end()) {
        if (version_it->is_string()) {
            parsed.version = version_it->get<std::string>();
        } else if (version_it->is_number_integer()) {
            parsed.version = std::to_string(version_it->get<std::int64_t>());
        }
    }
    const auto description_it = entry.find("description");
    if (description_it != entry.end() && description_it->is_string()) {
        parsed.description = description_it->get<std::string>();
    }
    const auto sha_it = entry.find("sha256");
    if (sha_it != entry.end() && sha_it->is_string()) {
        parsed.sha256 = sha_it->get<std::string>();
    }
    // The signature is type-checked unlike the other optional strings: a
    // present-but-malformed signature would otherwise be silently dropped,
    // which reads as "unsigned" when the publisher meant "signed". It is
    // verified fail-closed at install (see install_from_catalog).
    const auto signature_it = entry.find("signature");
    if (signature_it != entry.end()) {
        if (!signature_it->is_string()) {
            builder.fail("an entry's signature is not a string");
            return;
        }
        parsed.signature = signature_it->get<std::string>();
    }
    if (!read_source(entry, parsed.source, builder)) {
        return;
    }
    builder.entries.push_back(std::move(parsed));
}

// Reads one entry of the catalog's optional `revoked` array. Returns without
// appending when it is unusable (a problem is recorded), so a malformed
// revocation fails the whole catalog like a malformed entry does.
void read_revocation(const nlohmann::json &entry, Builder &builder)
{
    if (!entry.is_object()) {
        builder.fail("a revocation is not an object");
        return;
    }
    const auto id_it = entry.find("id");
    if (id_it == entry.end() || !id_it->is_string()
        || !core::is_namespaced_id(id_it->get<std::string>())) {
        builder.fail("a revocation's id is missing or not a namespaced id");
        return;
    }
    Revocation revocation;
    revocation.id = id_it->get<std::string>();
    const auto version_it = entry.find("version");
    if (version_it != entry.end()) {
        if (!version_it->is_string()) {
            builder.fail("a revocation's version is not a string");
            return;
        }
        revocation.version = version_it->get<std::string>();
    }
    const auto reason_it = entry.find("reason");
    if (reason_it != entry.end()) {
        if (!reason_it->is_string()) {
            builder.fail("a revocation's reason is not a string");
            return;
        }
        revocation.reason = reason_it->get<std::string>();
    }
    builder.revocations.push_back(std::move(revocation));
}

} // namespace

// The 128-hex signature field decoded to 64 raw bytes; empty on any non-hex
// input or a wrong length. read_entry has already type-checked the field, but
// a string of any shape may still be present.
static std::vector<std::uint8_t> decode_signature(std::string_view hex)
{
    if (hex.size() != 128) {
        return { };
    }
    const auto is_hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); };
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        return c - 'a' + 10;
    };
    std::vector<std::uint8_t> out(64, 0);
    for (std::size_t i = 0; i < 64; ++i) {
        const char hi = hex[2 * i];
        const char lo = hex[2 * i + 1];
        if (!is_hex(hi) || !is_hex(lo)) {
            return { };
        }
        out[i] = static_cast<std::uint8_t>((nibble(hi) << 4) | nibble(lo));
    }
    return out;
}

bool catalog_key_configured()
{
    for (const std::uint8_t byte : kCatalogPublicKey) {
        if (byte != 0) {
            return true;
        }
    }
    return false;
}

std::string signed_entry_payload(const CatalogEntry &entry)
{
    return entry.id + "\n" + entry.sha256.value_or("");
}

workspace::update::VerifyResult verify_entry_signature(const CatalogEntry &entry,
                                                       const workspace::update::PublicKey &key)
{
    if (!entry.signature || !entry.sha256) {
        return workspace::update::VerifyResult::Rejected;
    }
    const std::vector<std::uint8_t> signature = decode_signature(*entry.signature);
    if (signature.empty()) {
        return workspace::update::VerifyResult::Rejected;
    }
    const std::string payload = signed_entry_payload(entry);
    return workspace::update::verify_ed25519(
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(payload.data()),
                                          payload.size()),
            signature, key);
}

CatalogResult parse_catalog(std::string_view text)
{
    Builder builder;
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception &error) {
        builder.fail(std::string("the catalog is not valid JSON: ") + error.what());
        return CatalogResult{ std::nullopt, { }, std::move(builder.problems) };
    }

    if (!doc.is_object()) {
        builder.fail("the catalog is not a JSON object");
        return CatalogResult{ std::nullopt, { }, std::move(builder.problems) };
    }

    const auto entries_it = doc.find("entries");
    if (entries_it == doc.end() || !entries_it->is_array()) {
        builder.fail("`entries` is missing or not an array");
        return CatalogResult{ std::nullopt, { }, std::move(builder.problems) };
    }
    for (const nlohmann::json &entry : *entries_it) {
        read_entry(entry, builder);
    }

    // The optional `revoked` array: an empty/absent list is valid; a malformed
    // entry fails the whole catalog, the same all-or-nothing rule as `entries`.
    const auto revoked_it = doc.find("revoked");
    if (revoked_it != doc.end()) {
        if (!revoked_it->is_array()) {
            builder.fail("`revoked` is not an array");
        } else {
            for (const nlohmann::json &entry : *revoked_it) {
                read_revocation(entry, builder);
            }
        }
    }

    CatalogResult result;
    if (builder.problems.empty()) {
        result.entries = std::move(builder.entries);
        result.revocations = std::move(builder.revocations);
    } else {
        result.problems = std::move(builder.problems);
    }
    return result;
}

CatalogResult fetch_catalog(const std::string &url, const CatalogFetcher &fetcher)
{
    CatalogResult result;
    if (!fetcher) {
        result.problems.push_back("no catalog fetcher is wired");
        return result;
    }
    const std::optional<std::string> body = fetcher(url);
    if (!body) {
        result.problems.push_back("could not fetch the catalog: " + url);
        return result;
    }
    return parse_catalog(*body);
}

InstallResult install_from_catalog(const std::string &url, const std::string &id,
                                   const std::filesystem::path &store_root, Origin origin,
                                   const CatalogFetcher &fetcher)
{
    const CatalogResult catalog = fetch_catalog(url, fetcher);
    if (!catalog.entries) {
        InstallResult result;
        result.problems = catalog.problems;
        return result;
    }

    // Reconcile the store's revocation list with this catalog's (the catalog
    // is authoritative for its own revocations; see RevokedList.h), before the
    // entry is resolved so the list applies even when the requested id is
    // unknown. Best-effort, matching the advisory nature of a revocation
    // record: a failed write is reported on the result, never fatal.
    std::string persist_problem;
    if (!replace_revocations(store_root, catalog.revocations)) {
        persist_problem = "cannot persist the catalog's revocation list in the store";
    }

    for (const CatalogEntry &entry : *catalog.entries) {
        if (entry.id != id) {
            continue;
        }
        // Revocation gate, before any integrity or source work: an entry the
        // catalog itself revokes is refused outright, with the publisher's
        // reason when one is given.
        for (const Revocation &revocation : catalog.revocations) {
            if (revocation.id == entry.id && revocation_matches(revocation, entry.version)) {
                InstallResult result;
                result.problems.push_back(revocation.reason ? "the entry '" + id
                                                          + "' is revoked: " + *revocation.reason
                                                            : "the entry '" + id + "' is revoked");
                return result;
            }
        }
        // Integrity gates, before any source work. A signature is verified
        // fail-closed: with no crypto, no key, or a bad signature the install
        // is refused rather than proceeding unsigned. A declared digest is then
        // enforced by install_from_source against the materialized source.
        if (entry.signature) {
            if (!entry.sha256) {
                InstallResult result;
                result.problems.push_back("the entry is signed but declares no sha256; there is "
                                          "nothing to bind the signature to");
                return result;
            }
            const workspace::update::VerifyResult verdict =
                    verify_entry_signature(entry, kCatalogPublicKey);
            if (verdict == workspace::update::VerifyResult::Unavailable) {
                InstallResult result;
                result.problems.push_back("the entry is signed but signature verification is "
                                          "unavailable in this build");
                return result;
            }
            if (verdict != workspace::update::VerifyResult::Verified) {
                InstallResult result;
                result.problems.push_back(
                        catalog_key_configured()
                                ? "the entry's signature does not verify"
                                : "the entry is signed but no catalog signing key is configured "
                                  "(signature verification is pending)");
                return result;
            }
        }
        InstallResult result = install_from_source(entry.source, store_root, origin, entry.sha256);
        if (!persist_problem.empty()) {
            result.problems.push_back(persist_problem);
        }
        return result;
    }
    InstallResult result;
    if (!persist_problem.empty()) {
        result.problems.push_back(persist_problem);
    }
    result.problems.push_back("unknown extension id '" + id + "' in the catalog");
    return result;
}

nlohmann::json catalog_entries_json(const std::vector<CatalogEntry> &entries)
{
    nlohmann::json out = nlohmann::json::array();
    for (const CatalogEntry &entry : entries) {
        nlohmann::json source = nlohmann::json::object();
        source["type"] = source_type_name(entry.source.type);
        source["url"] = entry.source.url;
        if (!entry.source.ref.empty()) {
            source["ref"] = entry.source.ref;
        }
        if (!entry.source.subdir.empty()) {
            source["subdir"] = entry.source.subdir;
        }

        nlohmann::json item = nlohmann::json::object();
        item["id"] = entry.id;
        item["name"] = entry.name;
        item["version"] = entry.version;
        item["description"] = entry.description;
        item["source"] = std::move(source);
        item["sha256"] = entry.sha256 ? nlohmann::json(*entry.sha256) : nlohmann::json(nullptr);
        item["signature"] =
                entry.signature ? nlohmann::json(*entry.signature) : nlohmann::json(nullptr);
        out.push_back(std::move(item));
    }
    return out;
}

} // namespace genesis::extensions
