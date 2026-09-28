// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "extensions/Catalog.h"
#include "extensions/Install.h"
#include "extensions/Sources.h"

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const std::string &what)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

namespace ext = genesis::extensions;

const std::filesystem::path kBase =
        std::filesystem::temp_directory_path() / "genesis-catalog-tests";
const std::filesystem::path kStore = kBase / "store";

// A minimal, valid format-1 native manifest for the given id.
std::string native_manifest(const std::string &id)
{
    return "format = 1\n\n"
           "[extension]\n"
           "id = \""
            + id
            + "\"\n"
              "name = \"Fixture\"\n"
              "kind = \"native\"\n"
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
    return nlohmann::json{
        { "format", 1 }, { "entries", nlohmann::json::array({ entry }) }
    }.dump();
}

void test_parse_valid()
{
    const nlohmann::json doc{
        { "format", 1 },
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

void test_parse_refuses_newer_format()
{
    const nlohmann::json doc{ { "format", 2 }, { "entries", nlohmann::json::array() } };
    const ext::CatalogResult catalog = ext::parse_catalog(doc.dump());
    check(!catalog.entries.has_value(), "a newer format is refused");
    check(!catalog.problems.empty(), "and a problem is recorded");
}

void test_parse_rejects_bad_entry()
{
    const nlohmann::json doc{
        { "format", 1 },
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
        { "format", 1 },
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
    const nlohmann::json doc{ { "format", 1 },
                              { "entries",
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

} // namespace

int main()
{
    std::filesystem::remove_all(kBase); // clear a stale run
    std::filesystem::create_directories(kBase);

    test_parse_valid();
    test_parse_refuses_newer_format();
    test_parse_rejects_bad_entry();
    test_parse_rejects_missing_source();
    test_parse_rejects_invalid_json();
    test_fetch_without_fetcher();
    test_fetch_failure();
    test_catalog_entries_json_shape();
    test_install_from_catalog();
    test_install_unknown_id();

    std::filesystem::remove_all(kBase);

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
