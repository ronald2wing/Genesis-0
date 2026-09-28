// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

// The settings facade's catalog-URL suite: drives the marketplace URL's
// default-vs-persisted resolution and its config-dir round-trip headlessly.
// AppSettings is a facade over the four app controllers, but the catalog URL
// touches none of them, so a null-controller facade exercises the setting
// without any engine or GL dependency.

#include <cstdio>
#include <filesystem>
#include <string>

#include <QCoreApplication>
#include <QString>

#include "app/controllers/AppSettings.h"

#include "workspace/CacheSweep.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

void test_default_is_empty()
{
    AppSettings settings(nullptr, nullptr, nullptr, nullptr);
    check(settings.catalogUrl().isEmpty(), "a fresh facade has no catalog URL");
    check(AppSettings::kDefaultCatalogUrl.empty(),
          "the compile-time default is empty (no hosted feed yet)");
}

void test_url_roundtrip_and_persistence()
{
    const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "genesis-appsettings-test";
    std::filesystem::remove_all(dir);

    {
        AppSettings settings(nullptr, nullptr, nullptr, nullptr);
        settings.setConfigDir(dir);
        settings.setCatalogUrl("file:///srv/catalog.example.json");
    }
    // A second facade reading the same config dir restores the URL.
    AppSettings settings(nullptr, nullptr, nullptr, nullptr);
    settings.setConfigDir(dir);
    check(settings.catalogUrl() == "file:///srv/catalog.example.json",
          "the catalog URL survives a reload from the config dir");

    std::filesystem::remove_all(dir);
}

void test_default_vs_persisted_resolution()
{
    const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "genesis-appsettings-test2";
    std::filesystem::remove_all(dir);

    // No config dir: the value resolves to the (empty) default.
    AppSettings defaults(nullptr, nullptr, nullptr, nullptr);
    check(defaults.catalogUrl().isEmpty(),
          "without a config dir the URL resolves to the empty default");

    // Persist a value, then a fresh facade resolves to it instead of the default.
    {
        AppSettings writer(nullptr, nullptr, nullptr, nullptr);
        writer.setConfigDir(dir);
        writer.setCatalogUrl("https://example.com/catalog.json");
    }
    AppSettings persisted(nullptr, nullptr, nullptr, nullptr);
    persisted.setConfigDir(dir);
    check(persisted.catalogUrl() == "https://example.com/catalog.json",
          "a persisted URL overrides the empty default");

    std::filesystem::remove_all(dir);
}

void test_cache_cap_default_and_persistence()
{
    const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "genesis-appsettings-cachecap-test";
    std::filesystem::remove_all(dir);

    // No config dir: the cap resolves to the shared default (10 GiB).
    AppSettings defaults(nullptr, nullptr, nullptr, nullptr);
    check(defaults.cacheCapBytes() == genesis::workspace::kDefaultCacheCapBytes,
          "without a config dir the cap resolves to the 10 GiB default");

    // Persist a cap, then a fresh facade resolves to it.
    {
        AppSettings writer(nullptr, nullptr, nullptr, nullptr);
        writer.setConfigDir(dir);
        writer.setCacheCapBytes(2ULL * 1024 * 1024 * 1024);
    }
    AppSettings persisted(nullptr, nullptr, nullptr, nullptr);
    persisted.setConfigDir(dir);
    check(persisted.cacheCapBytes() == 2ULL * 1024 * 1024 * 1024,
          "a persisted cap survives a reload from the config dir");

    std::filesystem::remove_all(dir);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    test_default_is_empty();
    test_url_roundtrip_and_persistence();
    test_default_vs_persisted_resolution();
    test_cache_cap_default_and_persistence();

    return genesis::test::summary();
}
