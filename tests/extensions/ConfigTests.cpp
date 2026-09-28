// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "extensions/Config.h"
#include "extensions/Install.h"
#include "extensions/Origins.h"
#include "extensions/Sources.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

namespace ext = genesis::extensions;

// A scratch area under the system temp dir, cleared at the start of the run and
// removed at the end. `kStoreA` seeds extensions and exports; `kStoreB` imports.
const std::filesystem::path kBase = std::filesystem::temp_directory_path() / "genesis-config-tests";
const std::filesystem::path kStoreA = kBase / "store-a";
const std::filesystem::path kStoreB = kBase / "store-b";

// Writes a native-extension directory (an extension.toml plus a placeholder
// library file) and returns it.
std::filesystem::path write_native(const std::string &name, const std::string &manifest_text)
{
    const std::filesystem::path dir = kBase / name;
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir / "extension.toml");
        out << manifest_text;
    }
    {
        std::ofstream out(dir / "libfixture.so");
        out << "placeholder";
    }
    return dir;
}

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

// A profile document with the given settings and extensions arrays.
nlohmann::json profile(const nlohmann::json &settings, const nlohmann::json &extensions)
{
    return nlohmann::json{ { "appVersion", "0.1.0" },
                           { "settings", settings },
                           { "extensions", extensions } };
}

// Finds a reported entry by id, or returns nullopt.
const ext::ImportedExtension *find_entry(const ext::ImportReport &report, const std::string &id)
{
    for (const ext::ImportedExtension &entry : report.extensions) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

void test_export_captures_origin_and_source()
{
    // A Developer native install records both its origin and its Dir source
    // (the absolute path, since the default InstallSource normalizes an empty
    // url). The export must carry both.
    const auto source = write_native("exp", native_manifest("test.exp"));
    check(ext::install_extension(source, kStoreA, ext::Origin::Developer).ok(),
          "a Developer native install succeeds");

    const nlohmann::json doc = ext::export_profile(kStoreA, ext::ConfigSeam{ }, "0.1.0");
    check(!doc.contains("format"), "the profile carries no format key");
    check(doc["appVersion"] == "0.1.0", "the profile records the app version");
    check(doc["settings"].is_object() && doc["settings"].empty(),
          "an empty seam exports empty settings");
    check(doc["extensions"].is_array() && doc["extensions"].size() == 1,
          "one installed extension is exported");
    const nlohmann::json &entry = doc["extensions"][0];
    check(entry["id"] == "test.exp", "the entry names the id");
    check(entry["origin"] == "developer", "the entry carries the Developer origin");
    check(entry["source"].is_object(), "the entry carries a source record");
    check(entry["source"]["type"] == "dir", "the source is a Dir");
    check(entry["source"]["url"] == std::filesystem::absolute(source).string(),
          "the source url is the absolute install path");
}

void test_round_trip_reinstalls()
{
    // Export store A (already seeded by the previous test) and import into
    // store B: the Developer native extension re-installs from its recorded
    // path, and its source is re-recorded so a second export stays reproducible.
    const nlohmann::json doc = ext::export_profile(kStoreA, ext::ConfigSeam{ }, "0.1.0");
    const std::optional<ext::ImportReport> report =
            ext::import_profile(doc, kStoreB, ext::ConfigSeam{ });
    check(report.has_value(), "a valid profile imports");
    const ext::ImportedExtension *entry = find_entry(*report, "test.exp");
    check(entry != nullptr, "the report covers the imported id");
    if (entry != nullptr) {
        check(entry->status == "installed", "the Developer extension is installed");
    }
    check(ext::installed_packs(kStoreB).count("test.exp") == 1, "store B now holds the extension");

    // A second export from store B still carries a source record.
    const nlohmann::json re_exported = ext::export_profile(kStoreB, ext::ConfigSeam{ }, "0.1.0");
    check(re_exported["extensions"][0]["source"].is_object(),
          "the re-install re-recorded the source");
}

void test_import_rejects_non_objects()
{
    // A profile carrying an obsolete `format` key still imports (unknown keys
    // are ignored), while a non-object profile is refused outright.
    nlohmann::json with_format = profile(nlohmann::json::object(), nlohmann::json::array());
    with_format["format"] = 1;
    check(ext::import_profile(with_format, kStoreB, ext::ConfigSeam{ }).has_value(),
          "an unknown `format` key is tolerated");
    check(!ext::import_profile(nlohmann::json::array(), kStoreB, ext::ConfigSeam{ }),
          "a non-object profile is refused");
    check(!ext::import_profile("not a profile", kStoreB, ext::ConfigSeam{ }),
          "a scalar profile is refused");
}

void test_settings_applied_via_seam()
{
    bool proxies = false;
    int decode = -1;
    int interval = -1;
    std::string feed;
    ext::ConfigSeam seam;
    seam.set_proxies_enabled = [&](bool value) { proxies = value; };
    seam.set_decode_preference = [&](int value) { decode = value; };
    seam.set_scopes_interval_ms = [&](int value) { interval = value; };
    seam.set_feed_url = [&](const std::string &value) { feed = value; };

    const nlohmann::json settings{ { "proxiesEnabled", true },
                                   { "decodePreference", 2 },
                                   { "scopesIntervalMs", 250 },
                                   { "feedUrl", "https://example.com/feed" } };
    const std::optional<ext::ImportReport> report =
            ext::import_profile(profile(settings, nlohmann::json::array()), kStoreB, seam);
    check(report.has_value(), "a settings-only profile imports");
    check(proxies, "proxiesEnabled applied");
    check(decode == 2, "decodePreference applied");
    check(interval == 250, "scopesIntervalMs applied");
    check(feed == "https://example.com/feed", "feedUrl applied");

    bool found_applied = false;
    for (const std::string &line : report->settings) {
        if (line.find("applied proxiesEnabled") != std::string::npos) {
            found_applied = true;
        }
    }
    check(found_applied, "the report says the settings were applied");
}

void test_settings_skipped_without_seam()
{
    const nlohmann::json settings{ { "proxiesEnabled", true }, { "decodePreference", 1 } };
    const std::optional<ext::ImportReport> report = ext::import_profile(
            profile(settings, nlohmann::json::array()), kStoreB, ext::ConfigSeam{ });
    check(report.has_value(), "a settings-only profile imports");
    bool found_skip = false;
    for (const std::string &line : report->settings) {
        if (line.find("skipped proxiesEnabled") != std::string::npos) {
            found_skip = true;
        }
    }
    check(found_skip, "an unwired seam reports the setting as skipped");
}

void test_import_trust_rules()
{
    // Each rule below is asserted against a fresh store so the "already
    // installed" branch does not shadow the others. Only the Developer+Dir
    // entry (with an existing path) installs.

    const std::filesystem::path existing = write_native("rule-dev", native_manifest("rule.dev"));
    const std::filesystem::path installable =
            write_native("rule-install", native_manifest("rule.install"));
    const std::filesystem::path missing = kBase / "does-not-exist";

    // already installed: seed it into store B first.
    check(ext::install_extension(existing, kStoreB, ext::Origin::Developer).ok(),
          "pre-seed an already-installed id");

    nlohmann::json extensions = nlohmann::json::array();
    extensions.push_back({ { "id", "rule.dev" },
                           { "version", 1 },
                           { "origin", "developer" },
                           { "source", { { "type", "dir" }, { "url", existing.string() } } } });
    extensions.push_back({ { "id", "rule.nosrc" },
                           { "version", 1 },
                           { "origin", "developer" },
                           { "source", nullptr } });
    extensions.push_back({ { "id", "rule.builtin" },
                           { "version", 1 },
                           { "origin", "builtin" },
                           { "source", { { "type", "builtin" } } } });
    extensions.push_back(
            { { "id", "rule.git" },
              { "version", 1 },
              { "origin", "developer" },
              { "source", { { "type", "git" }, { "url", "/nonexistent/repository" } } } });
    extensions.push_back({ { "id", "rule.curated" },
                           { "version", 1 },
                           { "origin", "curated" },
                           { "source", { { "type", "dir" }, { "url", existing.string() } } } });
    extensions.push_back({ { "id", "rule.user" },
                           { "version", 1 },
                           { "origin", "user" },
                           { "source", { { "type", "dir" }, { "url", existing.string() } } } });
    extensions.push_back({ { "id", "rule.missing" },
                           { "version", 1 },
                           { "origin", "developer" },
                           { "source", { { "type", "dir" }, { "url", missing.string() } } } });
    extensions.push_back({ { "id", "rule.install" },
                           { "version", 1 },
                           { "origin", "developer" },
                           { "source", { { "type", "dir" }, { "url", installable.string() } } } });

    const std::optional<ext::ImportReport> report = ext::import_profile(
            profile(nlohmann::json::object(), extensions), kStoreB, ext::ConfigSeam{ });
    check(report.has_value(), "the trust-rules profile imports");

    const auto status = [&](const std::string &id) {
        const ext::ImportedExtension *entry = find_entry(*report, id);
        return entry != nullptr ? entry->status : std::string("<missing>");
    };

    // rule.dev is already installed (any version counts), so even with a valid
    // source it is skipped, not reinstalled.
    check(status("rule.dev") == "skipped", "an installed id is skipped");
    check(status("rule.nosrc") == "skipped", "a missing source is skipped");
    check(status("rule.builtin") == "skipped", "a Builtin source is skipped");
    check(status("rule.git") == "failed", "a Developer Git with an unreachable path is failed");
    check(status("rule.curated") == "skipped", "a Curated origin is skipped (no trusted source)");
    check(status("rule.user") == "failed", "a User origin is failed");
    check(status("rule.missing") == "failed", "a Developer Dir with a missing path is failed");
    check(status("rule.install") == "installed",
          "a Developer Dir with an existing path is installed");

    check(ext::installed_packs(kStoreB).count("rule.install") == 1,
          "the installable id lands in the store");
    check(ext::installed_packs(kStoreB).count("rule.missing") == 0,
          "the missing-path id does not land");
    check(ext::installed_packs(kStoreB).count("rule.nosrc") == 0,
          "the source-less id does not land");
}

} // namespace

int main()
{
    std::filesystem::remove_all(kBase); // clear a stale run
    std::filesystem::create_directories(kBase);

    test_export_captures_origin_and_source();
    test_round_trip_reinstalls();
    test_import_rejects_non_objects();
    test_settings_applied_via_seam();
    test_settings_skipped_without_seam();
    test_import_trust_rules();

    std::filesystem::remove_all(kBase);

    return genesis::test::summary();
}
