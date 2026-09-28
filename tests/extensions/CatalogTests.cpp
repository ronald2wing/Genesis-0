// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "extensions/Catalog.h"
#include "extensions/Install.h"
#include "extensions/RevokedList.h"
#include "extensions/SourceInstall.h"
#include "extensions/Sources.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

namespace ext = genesis::extensions;

const std::filesystem::path kBase =
        std::filesystem::temp_directory_path() / "genesis-catalog-tests";
const std::filesystem::path kStore = kBase / "store";

// A minimal, valid native manifest for the given id.
std::string native_manifest(const std::string &id)
{
    return "[extension]\n"
           "id = \""
            + id
            + "\"\n"
              "name = \"Fixture\"\n"
              "version = 1\n"
              "api = 1\n"
              "entry = \"libfixture.so\"\n";
}

std::filesystem::path write_native(const std::string &name, const std::string &id)
{
    const std::filesystem::path dir = kBase / name;
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir / "extension.toml");
        out << native_manifest(id);
    }
    {
        std::ofstream out(dir / "libfixture.so");
        out << "placeholder";
    }
    return dir;
}

// A one-entry catalog document for a Dir source.
std::string dir_catalog(const std::filesystem::path &dir)
{
    nlohmann::json entry{ { "id", "catalog.dir" },
                          { "name", "Catalog Dir" },
                          { "version", "1" },
                          { "description", "a dir-source entry" },
                          { "source", { { "type", "dir" }, { "url", dir.string() } } } };
    return nlohmann::json{ { "entries", nlohmann::json::array({ entry }) } }.dump();
}

// A one-entry catalog document for a Dir source, optionally pinning the entry's
// digest and/or signature. The entry id is passed in so each test can use a
// distinct id without the manifest id drifting from the catalog entry id.
std::string dir_catalog_with(const std::string &id, const std::filesystem::path &dir,
                             const std::optional<std::string> &sha256,
                             const std::optional<std::string> &signature = std::nullopt)
{
    nlohmann::json entry{ { "id", id },
                          { "name", "Catalog Dir" },
                          { "version", "1" },
                          { "description", "a dir-source entry" },
                          { "source", { { "type", "dir" }, { "url", dir.string() } } } };
    if (sha256) {
        entry["sha256"] = *sha256;
    }
    if (signature) {
        entry["signature"] = *signature;
    }
    return nlohmann::json{ { "entries", nlohmann::json::array({ entry }) } }.dump();
}

// A one-entry catalog whose `revoked` list names the entry (or another id),
// so the install path's revocation gate and persistence can be exercised.
std::string revoked_catalog(const std::string &id, const std::filesystem::path &dir,
                            const std::optional<std::string> &revoked_version,
                            const std::optional<std::string> &reason)
{
    nlohmann::json entry{ { "id", id },
                          { "name", "Catalog Dir" },
                          { "version", "1" },
                          { "description", "a dir-source entry" },
                          { "source", { { "type", "dir" }, { "url", dir.string() } } } };
    nlohmann::json revocation{ { "id", id } };
    if (revoked_version) {
        revocation["version"] = *revoked_version;
    }
    if (reason) {
        revocation["reason"] = *reason;
    }
    return nlohmann::json{
        { "entries", nlohmann::json::array({ entry }) },
        { "revoked", nlohmann::json::array({ revocation }) }
    }.dump();
}

void test_parse_valid()
{
    const nlohmann::json doc{
        { "entries",
          nlohmann::json::array({ nlohmann::json{ { "id", "a.dev" },
                                                  { "name", "A" },
                                                  { "version", "1.2.3" },
                                                  { "description", "the first" },
                                                  { "source",
                                                    { { "type", "git" },
                                                      { "url", "https://example.com/a.git" },
                                                      { "ref", "v1" },
                                                      { "subdir", "ext" } } } } }) }
    };
    const ext::CatalogResult catalog = ext::parse_catalog(doc.dump());
    check(catalog.entries.has_value(), "a valid catalog parses");
    if (catalog.entries) {
        check(catalog.entries->size() == 1, "one entry is read");
        const ext::CatalogEntry &entry = catalog.entries->front();
        check(entry.id == "a.dev", "the id is read");
        check(entry.name == "A", "the name is read");
        check(entry.version == "1.2.3", "the version is read");
        check(entry.source.type == ext::SourceType::Git, "the source is Git");
        check(entry.source.url == "https://example.com/a.git", "the url is read");
        check(entry.source.ref == "v1", "the ref is read");
        check(entry.source.subdir == "ext", "the subdir is read");
    }
}

void test_parse_tolerates_an_unknown_key()
{
    // An unknown key (here an obsolete `format`) is ignored: the reader is
    // look-up-only, so the catalog still parses.
    const nlohmann::json doc{ { "format", 2 }, { "entries", nlohmann::json::array() } };
    const ext::CatalogResult catalog = ext::parse_catalog(doc.dump());
    check(catalog.entries.has_value(), "an unknown top-level key is tolerated");
    check(catalog.problems.empty(), "and no problem is recorded");
}

void test_parse_rejects_bad_entry()
{
    const nlohmann::json doc{
        { "entries",
          nlohmann::json::array({ nlohmann::json{
                  { "id", "notnamespaced" },
                  { "name", "Bad" },
                  { "source", { { "type", "git" }, { "url", "https://example.com/x.git" } } } } }) }
    };
    const ext::CatalogResult catalog = ext::parse_catalog(doc.dump());
    check(!catalog.entries.has_value(), "an unnamespaced id refuses the catalog");
}

void test_parse_rejects_missing_source()
{
    const nlohmann::json doc{
        { "entries",
          nlohmann::json::array({ nlohmann::json{ { "id", "a.dev" }, { "name", "NoSource" } } }) }
    };
    const ext::CatalogResult catalog = ext::parse_catalog(doc.dump());
    check(!catalog.entries.has_value(), "a missing source refuses the catalog");
}

void test_parse_rejects_invalid_json()
{
    const ext::CatalogResult catalog = ext::parse_catalog("{ not json");
    check(!catalog.entries.has_value(), "invalid JSON is refused");
    check(!catalog.problems.empty(), "and a problem is recorded");
}

void test_parse_revoked_list()
{
    const nlohmann::json doc{ { "entries", nlohmann::json::array() },
                              { "revoked",
                                nlohmann::json::array({ nlohmann::json{ { "id", "a.dev" },
                                                                        { "version", "1.2" },
                                                                        { "reason", "pwned" } },
                                                        nlohmann::json{ { "id", "b.dev" } } }) } };
    const ext::CatalogResult catalog = ext::parse_catalog(doc.dump());
    check(catalog.entries.has_value(), "a catalog with a revoked list parses");
    check(catalog.revocations.size() == 2, "both revocations are read");
    check(catalog.revocations[0].id == "a.dev"
                  && catalog.revocations[0].version == std::optional<std::string>("1.2")
                  && catalog.revocations[0].reason == std::optional<std::string>("pwned"),
          "the version and reason are read");
    check(catalog.revocations[1].id == "b.dev" && !catalog.revocations[1].version,
          "a version-less revocation is read");
}

void test_parse_revoked_bad_entry_refuses()
{
    const nlohmann::json doc{ { "entries", nlohmann::json::array() },
                              { "revoked",
                                nlohmann::json::array({ nlohmann::json{ { "version", "1" } } }) } };
    const ext::CatalogResult catalog = ext::parse_catalog(doc.dump());
    check(!catalog.entries.has_value(), "a revoked entry without an id refuses the catalog");
    check(!catalog.problems.empty(), "and a problem is recorded");
}

void test_fetch_without_fetcher()
{
    const ext::CatalogResult catalog = ext::fetch_catalog("https://example.com/catalog.json", { });
    check(!catalog.entries.has_value(), "a fetch without a fetcher fails");
    check(!catalog.problems.empty(), "with a 'not wired' problem");
}

void test_fetch_failure()
{
    const ext::CatalogFetcher fetcher = [](const std::string &) -> std::optional<std::string> {
        return std::nullopt;
    };
    const ext::CatalogResult catalog =
            ext::fetch_catalog("https://example.com/catalog.json", fetcher);
    check(!catalog.entries.has_value(), "a failed fetch reports failure");
}

void test_catalog_entries_json_shape()
{
    const nlohmann::json doc{ { "entries",
                                nlohmann::json::array({ nlohmann::json{
                                        { "id", "a.dev" },
                                        { "name", "A" },
                                        { "version", "1" },
                                        { "source", { { "type", "dir" }, { "url", "/src/a" } } },
                                        { "sha256", "deadbeef" } } }) } };
    const ext::CatalogResult catalog = ext::parse_catalog(doc.dump());
    check(catalog.entries.has_value(), "the fixture parses");
    if (!catalog.entries) {
        return;
    }
    const nlohmann::json out = ext::catalog_entries_json(*catalog.entries);
    check(out.is_array() && out.size() == 1, "the reply is a one-entry array");
    check(out[0]["id"] == "a.dev", "the reply carries the id");
    check(out[0]["source"]["type"] == "dir", "the reply carries the source type");
    check(out[0]["source"]["url"] == "/src/a", "the reply carries the source url");
    check(out[0]["sha256"] == "deadbeef", "the reply carries the sha256");
    check(out[0]["signature"].is_null(), "an absent signature reads null in the reply");
}

void test_signature_roundtrip()
{
    const std::string signature(128, '0'); // a 64-byte Ed25519 signature, hex
    const nlohmann::json doc{ { "entries",
                                nlohmann::json::array({ nlohmann::json{
                                        { "id", "a.dev" },
                                        { "name", "A" },
                                        { "version", "1" },
                                        { "source", { { "type", "dir" }, { "url", "/src/a" } } },
                                        { "signature", signature } } }) } };
    const ext::CatalogResult catalog = ext::parse_catalog(doc.dump());
    check(catalog.entries.has_value(), "an entry with a signature parses");
    if (!catalog.entries) {
        return;
    }
    check(catalog.entries->front().signature == signature,
          "the signature is carried verbatim on the entry");
    const nlohmann::json out = ext::catalog_entries_json(*catalog.entries);
    check(out[0]["signature"] == signature, "the reply carries the signature");
}

void test_signature_rejects_non_string()
{
    const nlohmann::json doc{
        { "entries",
          nlohmann::json::array({ nlohmann::json{
                  { "id", "a.dev" },
                  { "name", "A" },
                  { "source", { { "type", "git" }, { "url", "https://e/x.git" } } },
                  { "signature", 42 } } }) }
    };
    const ext::CatalogResult catalog = ext::parse_catalog(doc.dump());
    check(!catalog.entries.has_value(), "a non-string signature refuses the catalog");
    check(!catalog.problems.empty(), "and a problem is recorded");
}

void test_sample_catalog_parses()
{
    // The shipped sample catalog must keep parsing through the real parser: it
    // is the on-disk example a publisher copies. Locate the repo root by
    // walking up from the test's cwd (build dir or runtime dir both reach it).
    std::filesystem::path dir = std::filesystem::current_path();
    std::filesystem::path sample;
    while (true) {
        std::error_code ec;
        const std::filesystem::path candidate = dir / "extensions" / "catalog.example.json";
        if (std::filesystem::is_regular_file(candidate, ec)) {
            sample = candidate;
            break;
        }
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir) {
            break;
        }
        dir = parent;
    }
    if (sample.empty()) {
        check(false, "the sample catalog is found");
        return;
    }

    std::ifstream in(sample);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const ext::CatalogResult catalog = ext::parse_catalog(text);
    check(catalog.entries.has_value(), "the sample catalog parses");
    if (!catalog.entries) {
        for (const std::string &problem : catalog.problems) {
            std::printf("  (problem: %s)\n", problem.c_str());
        }
        return;
    }
    check(catalog.entries->size() == 3, "the sample lists three entries");
    bool saw_git = false;
    bool saw_dir = false;
    for (const ext::CatalogEntry &entry : *catalog.entries) {
        check(entry.id.rfind("example.", 0) == 0, "the sample id is example-namespaced");
        check(entry.sha256.has_value(), "the sample entry carries a sha256");
        check(entry.signature.has_value(), "the sample entry carries a signature");
        saw_git = saw_git || entry.source.type == ext::SourceType::Git;
        saw_dir = saw_dir || entry.source.type == ext::SourceType::Dir;
    }
    check(saw_git, "the sample has a git-source entry");
    check(saw_dir, "the sample has a dir-source entry");
}

void test_install_from_catalog()
{
    const std::filesystem::path dir = write_native("catalog-src", "catalog.dir");
    const ext::CatalogFetcher fetcher = [&](const std::string &) -> std::optional<std::string> {
        return dir_catalog(dir);
    };
    const ext::InstallResult result =
            ext::install_from_catalog("https://example.com/catalog.json", "catalog.dir", kStore,
                                      ext::Origin::Developer, fetcher);
    check(result.ok(), "a dir-source catalog entry installs");
    check(ext::installed_packs(kStore).count("catalog.dir") == 1,
          "the catalog entry lands in the store");
}

void test_install_unknown_id()
{
    const std::filesystem::path dir = write_native("catalog-src2", "catalog.dir2");
    const ext::CatalogFetcher fetcher = [&](const std::string &) -> std::optional<std::string> {
        return dir_catalog(dir);
    };
    const ext::InstallResult result =
            ext::install_from_catalog("https://example.com/catalog.json", "catalog.missing", kStore,
                                      ext::Origin::Developer, fetcher);
    check(!result.ok(), "an unknown id is refused");
    check(!result.problems.empty(), "with a 'not in catalog' problem");
}

void test_install_digest_match()
{
    const std::filesystem::path dir = write_native("catalog-digest-ok", "catalog.digest");
    const std::optional<std::string> digest = ext::tree_sha256(dir);
    check(digest.has_value(), "the fixture directory has a content digest");
    const ext::CatalogFetcher fetcher = [&](const std::string &) -> std::optional<std::string> {
        return dir_catalog_with("catalog.digest", dir, digest);
    };
    const ext::InstallResult result =
            ext::install_from_catalog("https://example.com/catalog.json", "catalog.digest", kStore,
                                      ext::Origin::Developer, fetcher);
    check(result.ok(), "a dir-source entry whose digest matches installs");
    check(ext::installed_packs(kStore).count("catalog.digest") == 1,
          "the digest-verified entry lands in the store");
}

void test_install_digest_mismatch_refused()
{
    const std::filesystem::path dir = write_native("catalog-digest-bad", "catalog.digestbad");
    const ext::CatalogFetcher fetcher = [&](const std::string &) -> std::optional<std::string> {
        return dir_catalog_with("catalog.digestbad", dir, std::string(64, '0'));
    };
    const ext::InstallResult result =
            ext::install_from_catalog("https://example.com/catalog.json", "catalog.digestbad",
                                      kStore, ext::Origin::Developer, fetcher);
    check(!result.ok(), "a dir-source entry whose digest does not match is refused");
    check(ext::installed_packs(kStore).count("catalog.digestbad") == 0,
          "and nothing lands in the store");
}

void test_install_malformed_sha256_refused()
{
    const std::filesystem::path dir = write_native("catalog-sha-bad", "catalog.shabad");
    const ext::CatalogFetcher fetcher = [&](const std::string &) -> std::optional<std::string> {
        return dir_catalog_with("catalog.shabad", dir, std::string("not-hex"));
    };
    const ext::InstallResult result =
            ext::install_from_catalog("https://example.com/catalog.json", "catalog.shabad", kStore,
                                      ext::Origin::Developer, fetcher);
    check(!result.ok(), "an entry with a malformed sha256 is refused");
}

void test_install_signed_entry_refused()
{
    // A signed entry is refused fail-closed: in the default build the crypto is
    // not compiled in, so verification is Unavailable; with crypto compiled the
    // placeholder (all-zeros) key is not configured, so it is rejected. Either
    // way a signed entry never installs unsigned.
    const std::filesystem::path dir = write_native("catalog-signed", "catalog.signed");
    const std::optional<std::string> digest = ext::tree_sha256(dir);
    const ext::CatalogFetcher fetcher = [&](const std::string &) -> std::optional<std::string> {
        return dir_catalog_with("catalog.signed", dir, digest, std::string(128, '0'));
    };
    const ext::InstallResult result =
            ext::install_from_catalog("https://example.com/catalog.json", "catalog.signed", kStore,
                                      ext::Origin::Developer, fetcher);
    check(!result.ok(), "a signed entry is refused when it cannot be verified");
    check(ext::installed_packs(kStore).count("catalog.signed") == 0,
          "and nothing lands in the store");
}

void test_install_signed_without_digest_refused()
{
    const std::filesystem::path dir = write_native("catalog-sig-nodig", "catalog.signodig");
    const ext::CatalogFetcher fetcher = [&](const std::string &) -> std::optional<std::string> {
        return dir_catalog_with("catalog.signodig", dir, std::nullopt, std::string(128, '0'));
    };
    const ext::InstallResult result =
            ext::install_from_catalog("https://example.com/catalog.json", "catalog.signodig",
                                      kStore, ext::Origin::Developer, fetcher);
    check(!result.ok(), "a signed entry with no sha256 is refused");
}

void test_install_refuses_revoked_entry()
{
    const std::filesystem::path dir = write_native("catalog-revoked", "catalog.revoked");
    const ext::CatalogFetcher fetcher = [&](const std::string &) -> std::optional<std::string> {
        return revoked_catalog("catalog.revoked", dir, std::nullopt, std::string("malware"));
    };
    const ext::InstallResult result =
            ext::install_from_catalog("https://example.com/catalog.json", "catalog.revoked", kStore,
                                      ext::Origin::Developer, fetcher);
    check(!result.ok(), "an entry the catalog revokes is refused");
    check(ext::installed_packs(kStore).count("catalog.revoked") == 0,
          "and nothing lands in the store");
    check(!result.problems.empty() && result.problems.front().find("malware") != std::string::npos,
          "with the publisher's reason in the problem");
}

void test_install_version_scoped_revocation_only_blocks_that_version()
{
    // The entry is version 1; a revocation naming version 2 does not match, so
    // the install proceeds. The revocation is still persisted to the store.
    const std::filesystem::path dir = write_native("catalog-revoked-v2", "catalog.revv2");
    const ext::CatalogFetcher fetcher = [&](const std::string &) -> std::optional<std::string> {
        return revoked_catalog("catalog.revv2", dir, std::string("2"), std::nullopt);
    };
    const ext::InstallResult result =
            ext::install_from_catalog("https://example.com/catalog.json", "catalog.revv2", kStore,
                                      ext::Origin::Developer, fetcher);
    check(result.ok(), "a revocation for another version does not block the install");
    check(ext::revoked_at(kStore, "catalog.revv2", "2").has_value(),
          "the version-scoped revocation is still persisted");
    check(!ext::revoked_at(kStore, "catalog.revv2", "1").has_value(),
          "and does not match the installed version");
}

void test_later_catalog_clears_revocation()
{
    // A catalog that revokes its entry refuses the install and persists the
    // revocation; a later catalog without the entry revokes nothing and the
    // store list is cleared (the catalog is authoritative), so the same id can
    // then install.
    const std::filesystem::path dir = write_native("catalog-unrevoke", "catalog.unrevoke");
    {
        const ext::CatalogFetcher revoking =
                [&](const std::string &) -> std::optional<std::string> {
            return revoked_catalog("catalog.unrevoke", dir, std::nullopt, std::string("bad"));
        };
        const ext::InstallResult refused =
                ext::install_from_catalog("https://example.com/catalog.json", "catalog.unrevoke",
                                          kStore, ext::Origin::Developer, revoking);
        check(!refused.ok(), "the revoking catalog refuses the install");
        check(ext::revoked_at(kStore, "catalog.unrevoke", "1").has_value(),
              "and persists the revocation");
    }
    {
        const ext::CatalogFetcher clear = [&](const std::string &) -> std::optional<std::string> {
            return dir_catalog_with("catalog.unrevoke", dir, std::nullopt);
        };
        const ext::InstallResult install =
                ext::install_from_catalog("https://example.com/catalog.json", "catalog.unrevoke",
                                          kStore, ext::Origin::Developer, clear);
        check(install.ok(), "the later catalog installs the id it no longer revokes");
        check(!ext::revoked_at(kStore, "catalog.unrevoke", "1").has_value(),
              "and the revocation is cleared from the store");
    }
}

} // namespace

int main()
{
    std::filesystem::remove_all(kBase); // clear a stale run
    std::filesystem::create_directories(kBase);

    test_parse_valid();
    test_parse_tolerates_an_unknown_key();
    test_parse_rejects_bad_entry();
    test_parse_rejects_missing_source();
    test_parse_rejects_invalid_json();
    test_parse_revoked_list();
    test_parse_revoked_bad_entry_refuses();
    test_fetch_without_fetcher();
    test_fetch_failure();
    test_catalog_entries_json_shape();
    test_signature_roundtrip();
    test_signature_rejects_non_string();
    test_sample_catalog_parses();
    test_install_from_catalog();
    test_install_unknown_id();
    test_install_digest_match();
    test_install_digest_mismatch_refused();
    test_install_malformed_sha256_refused();
    test_install_signed_entry_refused();
    test_install_signed_without_digest_refused();
    test_install_refuses_revoked_entry();
    test_install_version_scoped_revocation_only_blocks_that_version();
    test_later_catalog_clears_revocation();

    std::filesystem::remove_all(kBase);

    return genesis::test::summary();
}
