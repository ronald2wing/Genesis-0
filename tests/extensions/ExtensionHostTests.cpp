// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "extensions/Consent.h"
#include "extensions/ExtensionHost.h"
#include "extensions/Install.h"
#include "extensions/ExtensionManifest.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

bool contains(const std::string &haystack, const std::string &needle)
{
    return haystack.find(needle) != std::string::npos;
}

namespace ext = genesis::extensions;

// The fixture libraries are built side by side with this executable in the
// runtime output directory; resolve that directory from the running binary so
// the suite needs no path baked in at configure time.
std::filesystem::path fixture_dir()
{
    std::error_code ec;
    const std::filesystem::path exe = std::filesystem::canonical("/proc/self/exe", ec);
    return ec ? std::filesystem::path{ } : exe.parent_path();
}

std::filesystem::path fixture(const std::string &name)
{
    return fixture_dir() / ("libextension_fixture_" + name + ".so");
}

// A per-test install store, cleared and recreated so tests never see each
// other's on-disk state.
const std::filesystem::path kBase =
        std::filesystem::temp_directory_path() / "genesis-extension-scan-tests";

std::filesystem::path store_for(const std::string &name)
{
    const std::filesystem::path store = kBase / name;
    std::filesystem::remove_all(store);
    std::filesystem::create_directories(store);
    return store;
}

// A host whose call_json answers "version" with a fixed reply and whose log
// collects messages, so invoke round-trips and refusal lines are observable.
struct CollectingHost
{
    std::vector<std::string> logs;

    ext::NativeExtension::Host build()
    {
        ext::NativeExtension::Host host;
        host.call_json = [](const std::string &method, const std::string &, std::string &out) {
            if (method == "version") {
                out = "pong";
                return 0;
            }
            return -1;
        };
        host.log = [this](const std::string &message) { logs.push_back(message); };
        return host;
    }
};

// A valid native-extension manifest for the fixture library (whose descriptor
// reports id "ext.fixture"), with one menu and one panel.
std::string native_manifest(const std::string &id, std::uint32_t version,
                            const std::string &entry = "libfixture.so")
{
    std::string text = "[extension]\n";
    text += "id = \"" + id + "\"\n";
    text += "name = \"Fixture\"\n";
    text += "version = " + std::to_string(version) + "\n";
    text += "api = 1\n";
    text += "entry = \"" + entry + "\"\n";
    text += "\n[[menu]]\npath = \"Tools/Fixture\"\nlabel = \"Fixture\"\n"
            "action = \"greet\"\n";
    text += "\n[[panel]]\nlabel = \"Fixture Panel\"\nqml = \"FixturePanel.qml\"\n";
    text += "\n[[dialog]]\nlabel = \"Fixture Dialog\"\nqml = \"FixtureDialog.qml\"\n"
            "menu_path = \"Tools/Fixture\"\n";
    return text;
}

// A data-only manifest: no `entry`, so the extension contributes packs/panels
// but no library. `dependencies` names the extension ids this one depends on
// (empty for none).
std::string data_only_manifest(const std::string &id, std::uint32_t version,
                               const std::vector<std::string> &dependencies = { })
{
    std::string text = "[extension]\n";
    text += "id = \"" + id + "\"\n";
    text += "name = \"Data " + id + "\"\n";
    text += "version = " + std::to_string(version) + "\n";
    if (!dependencies.empty()) {
        text += "requires = [";
        for (std::size_t i = 0; i < dependencies.size(); ++i) {
            if (i != 0) {
                text += ", ";
            }
            text += "\"" + dependencies[i] + "\"";
        }
        text += "]\n";
    }
    return text;
}

// A data-only manifest for override-resolution tests: no `entry`, so it loads
// with no library. `overrides` are the `[extension]` `overrides` array entries
// (empty for none); `body` is the `[[panel]]`/`[[dialog]]` tables appended
// after the `[extension]` table, so capability ids can be set explicitly.
std::string override_manifest(const std::string &id, const std::vector<std::string> &overrides,
                              const std::string &body)
{
    std::string text = "[extension]\n";
    text += "id = \"" + id + "\"\n";
    text += "name = \"Data " + id + "\"\n";
    text += "version = 1\n";
    if (!overrides.empty()) {
        text += "overrides = [";
        for (std::size_t i = 0; i < overrides.size(); ++i) {
            if (i != 0) {
                text += ", ";
            }
            text += "\"" + overrides[i] + "\"";
        }
        text += "]\n";
    }
    text += body;
    return text;
}

// Writes a store entry `<store>/<id>/<version>/extension.toml`, optionally
// copying a compiled library beside it and creating a `packs/` directory.
std::filesystem::path write_extension(const std::filesystem::path &store, const std::string &id,
                                      std::uint32_t version, const std::string &manifest_text,
                                      const std::string &entry = "libfixture.so",
                                      const std::filesystem::path &library = { },
                                      bool with_packs = false)
{
    const std::filesystem::path root = store / id / std::to_string(version);
    std::filesystem::create_directories(root);
    {
        std::ofstream out(root / "extension.toml");
        out << manifest_text;
    }
    if (!library.empty()) {
        std::filesystem::copy_file(library, root / entry,
                                   std::filesystem::copy_options::overwrite_existing);
    }
    if (with_packs) {
        std::filesystem::create_directories(root / "packs");
    }
    return root;
}

// Writes `<store>/origins.json` with the given raw JSON (a flat id -> origin
// object), the store-level map the host reads to decide each extension's
// origin. Absent, every id falls back to Developer.
void write_origins(const std::filesystem::path &store, const std::string &json_text)
{
    std::ofstream out(store / "origins.json");
    out << json_text;
}

// Writes `<store>/disabled.json` listing `id`, the Builtin/Curated disable
// list the host reads to mark an id visible-but-not-loaded.
void write_disabled(const std::filesystem::path &store, const std::string &id)
{
    std::ofstream out(store / "disabled.json");
    out << "{\"ids\": [\"" << id << "\"]}\n";
}

// Writes `<store>/disabled.json` listing `id` with a recorded reason (as the
// Manager's dependent-cascade writes it).
void write_disabled_with_reason(const std::filesystem::path &store, const std::string &id,
                                const std::string &reason)
{
    std::ofstream out(store / "disabled.json");
    out << "{\"ids\": [\"" << id << "\"], \"reasons\": {\"" << id << "\": \"" << reason << "\"}}\n";
}

// Writes `<store>/revoked.json` listing `id` with an optional reason, the
// store-level revocation list the host reads to mark an id visible-but-revoked.
void write_revoked(const std::filesystem::path &store, const std::string &id,
                   const std::optional<std::string> &reason = std::nullopt)
{
    std::string text = "{\"revocations\": [{\"id\": \"" + id + "\"";
    if (reason) {
        text += ", \"reason\": \"" + *reason + "\"";
    }
    text += "}]}\n";
    std::ofstream out(store / "revoked.json");
    out << text;
}

// Writes a native-extension source directory (extension.toml + the fixture
// library) that install_extension copies into a store, mirroring what the CLI's
// `extensions install` does. The manifest id is "ext.fixture" because that is
// the id the fixture library reports, so the load cross-check passes.
std::filesystem::path write_source(const std::string &name, std::uint32_t version)
{
    const std::filesystem::path dir = kBase / name;
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir / "extension.toml");
        out << native_manifest("ext.fixture", version);
    }
    std::filesystem::copy_file(fixture("ok"), dir / "libfixture.so",
                               std::filesystem::copy_options::overwrite_existing);
    return dir;
}

void test_highest_version_per_id()
{
    const std::filesystem::path store = store_for("highest_version");
    write_extension(store, "ext.fixture", 1, native_manifest("ext.fixture", 1), "libfixture.so",
                    fixture("ok"));
    write_extension(store, "ext.fixture", 3, native_manifest("ext.fixture", 3), "libfixture.so",
                    fixture("ok"));
    ext::Consent(store).grant("ext.fixture");

    CollectingHost collector;
    ext::ExtensionHost host(store, collector.build());
    host.scan();

    check(host.records().size() == 1, "two versions collapse to one loaded extension");
    check(host.refusals().empty(), "no refusal for a consented extension");
    if (host.records().size() == 1) {
        check(host.records()[0].version == 3, "the highest version directory wins");
    }
}

void test_developer_consent_gating()
{
    const std::filesystem::path store = store_for("consent_gating");
    write_extension(store, "ext.fixture", 1, native_manifest("ext.fixture", 1), "libfixture.so",
                    fixture("ok"));

    CollectingHost collector;
    ext::ExtensionHost host(store, collector.build());
    host.scan();
    check(host.records().empty(), "no consent means nothing loads");
    check(host.refusals().size() == 1, "the unconsented id is refused");
    if (host.refusals().size() == 1) {
        check(contains(host.refusals()[0], "no recorded consent"),
              "the refusal names the missing consent");
    }

    ext::Consent(store).grant("ext.fixture");
    host.scan();
    check(host.records().size() == 1, "recorded consent lets it load");
    check(host.refusals().empty(), "and the refusal clears");
}

void test_bad_manifest_refused_without_log()
{
    const std::filesystem::path store = store_for("bad_manifest");
    // A manifest whose id is not namespaced: unusable, and never logged.
    const std::string bad = "[extension]\nid = \"badid\"\n"
                            "name = \"Bad\"\nversion = 1\n"
                            "api = 1\nentry = \"libfixture.so\"\n";
    write_extension(store, "badid", 1, bad, "libfixture.so", fixture("ok"));

    CollectingHost collector;
    ext::ExtensionHost host(store, collector.build());
    host.scan();
    check(host.records().empty(), "an unusable manifest loads nothing");
    check(host.refusals().size() == 1, "the manifest refusal is reported");
    if (host.refusals().size() == 1) {
        check(contains(host.refusals()[0], "badid"), "the refusal names the id");
    }
    check(collector.logs.empty(), "a manifest refusal is reported, not logged");
}

void test_missing_library_refused_and_logged()
{
    const std::filesystem::path store = store_for("missing_library");
    // A valid manifest whose entry names a library that is not installed.
    write_extension(store, "ext.fixture", 1, native_manifest("ext.fixture", 1, "libmissing.so"),
                    "libmissing.so");
    ext::Consent(store).grant("ext.fixture");

    CollectingHost collector;
    ext::ExtensionHost host(store, collector.build());
    host.scan();
    check(host.records().empty(), "a missing library loads nothing");
    check(host.refusals().size() == 1, "the load failure is refused");
    if (host.refusals().size() == 1) {
        check(contains(host.refusals()[0], "cannot load the library"),
              "the refusal names the load failure");
    }
    check(!collector.logs.empty(), "a library refusal is logged");
    if (!collector.logs.empty()) {
        check(contains(collector.logs.front(), "[ext:ext.fixture]"),
              "the log line is re-prefixed with the extension id");
    }
}

void test_menus_panels_and_pack_root()
{
    const std::filesystem::path store = store_for("contributions");
    write_extension(store, "ext.fixture", 2, native_manifest("ext.fixture", 2), "libfixture.so",
                    fixture("ok"), /*with_packs=*/true);
    ext::Consent(store).grant("ext.fixture");

    CollectingHost collector;
    ext::ExtensionHost host(store, collector.build());
    host.scan();
    check(host.records().size() == 1, "the extension loads");
    if (host.records().size() != 1) {
        return;
    }

    const ext::ExtensionRecord &extension = host.records()[0];
    check(extension.id == "ext.fixture", "the id round-trips");
    check(extension.name == "Fixture", "the name round-trips");
    check(extension.menus.size() == 1, "one menu is collected");
    if (extension.menus.size() == 1) {
        check(extension.menus[0].path == "Tools/Fixture", "the menu path round-trips");
        check(extension.menus[0].action && *extension.menus[0].action == "greet",
              "the menu action round-trips");
    }
    check(extension.panels.size() == 1, "one panel is collected");
    if (extension.panels.size() == 1) {
        check(extension.panels[0].qml == "FixturePanel.qml", "the panel qml round-trips");
        check(extension.panels[0].url.rfind("file://", 0) == 0, "the panel url is a file:// URL");
        check(contains(extension.panels[0].url, "FixturePanel.qml"),
              "the panel url names the qml file");
    }
    check(extension.dialogs.size() == 1, "one dialog is collected");
    if (extension.dialogs.size() == 1) {
        check(extension.dialogs[0].qml == "FixtureDialog.qml", "the dialog qml round-trips");
        check(extension.dialogs[0].url.rfind("file://", 0) == 0, "the dialog url is a file:// URL");
        check(contains(extension.dialogs[0].url, "FixtureDialog.qml"),
              "the dialog url names the qml file");
        check(extension.dialogs[0].menu_path && *extension.dialogs[0].menu_path == "Tools/Fixture",
              "the dialog menu_path round-trips");
    }
    check(!extension.pack_root.empty(), "the packs directory is collected");
    check(host.pack_roots().size() == 1, "pack_roots lists the one root");
}

void test_curated_origin_loads_without_consent()
{
    const std::filesystem::path store = store_for("curated_origin");
    write_extension(store, "ext.fixture", 1, native_manifest("ext.fixture", 1), "libfixture.so",
                    fixture("ok"));
    write_origins(store, "{\"ext.fixture\": \"curated\"}");

    CollectingHost collector;
    ext::ExtensionHost host(store, collector.build());
    host.scan();
    check(host.records().size() == 1, "a Curated extension loads without consent");
    check(host.refusals().empty(), "and nothing is refused");
    check(host.origin_of("ext.fixture") == ext::Origin::Curated,
          "origin_of reports the recorded origin");
    if (host.records().size() == 1) {
        check(host.records()[0].origin == ext::Origin::Curated,
              "the record carries the Curated origin");
        check(!host.records()[0].consented, "a Curated extension never records consent");
        check(host.records()[0].state == ext::ExtensionState::Enabled, "the record is enabled");
    }
}

void test_installed_origin_drives_consent()
{
    // The install -> scan round trip: install_extension records the chosen
    // origin (Install.cpp -> Origins), and the host's scan reads it back to
    // gate the load. A Curated install loads without consent; a Developer
    // install is consent-gated.
    const std::filesystem::path curated_store = store_for("installed_curated");
    check(ext::install_extension(write_source("src_curated", 1), curated_store,
                                 ext::Origin::Curated)
                  .ok(),
          "a Curated install succeeds");

    CollectingHost curated_collector;
    ext::ExtensionHost curated(curated_store, curated_collector.build());
    curated.scan();
    check(curated.records().size() == 1, "a Curated-installed extension loads without consent");
    check(curated.refusals().empty(), "and nothing is refused");
    if (curated.records().size() == 1) {
        check(curated.records()[0].origin == ext::Origin::Curated,
              "the record carries the Curated origin");
    }

    const std::filesystem::path dev_store = store_for("installed_developer");
    check(ext::install_extension(write_source("src_dev", 1), dev_store, ext::Origin::Developer)
                  .ok(),
          "a Developer install succeeds");

    CollectingHost dev_collector;
    ext::ExtensionHost dev(dev_store, dev_collector.build());
    dev.scan();
    check(dev.records().empty(), "a Developer-installed extension does not load without consent");
    check(dev.failed().size() == 1, "it is refused");
    if (dev.failed().size() == 1) {
        check(contains(dev.failed()[0].reason, "no recorded consent"), "with the consent refusal");
    }

    ext::Consent(dev_store).grant("ext.fixture");
    dev.scan();
    check(dev.records().size() == 1, "recorded consent loads the Developer-installed extension");
    check(dev.refusals().empty(), "and the refusal clears");
}

void test_default_origin_is_developer()
{
    const std::filesystem::path store = store_for("default_origin");
    write_extension(store, "ext.fixture", 1, native_manifest("ext.fixture", 1), "libfixture.so",
                    fixture("ok"));
    // No origins.json: the id falls back to Developer, so no consent -> failed.

    CollectingHost collector;
    ext::ExtensionHost host(store, collector.build());
    host.scan();
    check(host.records().empty(), "no origin map and no consent loads nothing");
    check(host.origin_of("ext.fixture") == ext::Origin::Developer,
          "origin_of defaults to Developer");
    check(host.failed().size() == 1, "the unconsented Developer id is failed");
    if (host.failed().size() == 1) {
        check(host.failed()[0].state == ext::ExtensionState::Failed, "the record is failed");
        check(contains(host.failed()[0].reason, "no recorded consent"),
              "the reason names the missing consent");
        check(!host.failed()[0].consented, "no consent is recorded");
    }
}

void test_builtin_disabled_is_listed_not_loaded()
{
    const std::filesystem::path store = store_for("disabled");
    write_extension(store, "ext.fixture", 1, native_manifest("ext.fixture", 1), "libfixture.so",
                    fixture("ok"));
    write_origins(store, "{\"ext.fixture\": \"builtin\"}");
    write_disabled(store, "ext.fixture");

    CollectingHost collector;
    ext::ExtensionHost host(store, collector.build());
    host.scan();
    check(host.records().empty(), "a disabled extension is not loaded");
    check(host.disabled().size() == 1, "it is listed as disabled");
    check(host.refusals().empty(), "and is not a refusal");
    if (host.disabled().size() == 1) {
        check(host.disabled()[0].state == ext::ExtensionState::Disabled, "the record is disabled");
        check(host.disabled()[0].reason.empty(), "a disabled record has no reason");
        check(host.disabled()[0].origin == ext::Origin::Builtin,
              "the record carries the Builtin origin");
    }
}

void test_user_origin_is_failed()
{
    const std::filesystem::path store = store_for("user_origin");
    write_extension(store, "ext.fixture", 1, native_manifest("ext.fixture", 1), "libfixture.so",
                    fixture("ok"));
    write_origins(store, "{\"ext.fixture\": \"user\"}");

    CollectingHost collector;
    ext::ExtensionHost host(store, collector.build());
    host.scan();
    check(host.records().empty(), "a User extension is never loaded");
    check(host.failed().size() == 1, "it is listed as failed");
    if (host.failed().size() == 1) {
        check(host.failed()[0].origin == ext::Origin::User, "the record carries the User origin");
        check(contains(host.failed()[0].reason, "not trusted"),
              "the reason names the untrusted origin");
    }
}

void test_invoke_routing_and_prefixed_callbacks()
{
    const std::filesystem::path store = store_for("invoke");
    write_extension(store, "ext.fixture", 1, native_manifest("ext.fixture", 1), "libfixture.so",
                    fixture("ok"));
    ext::Consent(store).grant("ext.fixture");

    CollectingHost collector;
    ext::ExtensionHost host(store, collector.build());
    host.scan();
    check(host.records().size() == 1, "the extension loads");

    const auto result = host.invoke("ext.fixture", "greet", "{}");
    check(result.has_value(), "invoke routes to the loaded extension");
    if (result) {
        check(*result == "greet:pong", "invoke echoes the action and the host's version reply");
    }

    const auto missing = host.invoke("ext.other", "greet", "{}");
    check(!missing.has_value(), "invoking an unknown id fails cleanly");

    // The notification callbacks reach the shared log sink re-prefixed.
    host.on_project_opened("/tmp/project.json");
    host.shutdown();
    check(collector.logs.size() == 2, "both callbacks reach the log sink");
    if (collector.logs.size() == 2) {
        check(contains(collector.logs[0], "[ext:ext.fixture] fixture: project opened"),
              "on_project_opened is re-prefixed with the id");
        check(contains(collector.logs[1], "[ext:ext.fixture] fixture: shutdown"),
              "on_shutdown is re-prefixed with the id");
    }
}

void test_shipped_example_manifest_parses()
{
    // The shipped hello-extension example must keep parsing: it is the one
    // on-disk manifest a developer copies. Locate the repo root by walking up
    // from the test's cwd (build dir or runtime dir both reach it).
    std::filesystem::path dir = std::filesystem::current_path();
    std::filesystem::path example;
    while (true) {
        std::error_code ec;
        const std::filesystem::path candidate =
                dir / "examples" / "hello-extension" / "extension.toml";
        if (std::filesystem::is_regular_file(candidate, ec)) {
            example = candidate;
            break;
        }
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir) {
            break;
        }
        dir = parent;
    }
    if (example.empty()) {
        check(false, "the hello-extension example manifest is found");
        return;
    }

    const ext::NativeLoadResult loaded = ext::load_native_manifest_file(example);
    check(loaded.manifest.has_value(), "the shipped example parses cleanly");
    if (!loaded.manifest) {
        std::printf("  (problems: %zu)\n", loaded.problems.size());
        return;
    }

    const ext::ExtensionManifest &manifest = *loaded.manifest;
    check(manifest.id == "example.hello", "the example id is read");
    check(manifest.name == "Hello Extension", "the example name is read");
    check(manifest.version == 1, "the example version is read");
    check(manifest.api == 1, "the example api is read");
    check(manifest.entry && *manifest.entry == "libhello.so", "the example entry is read");
    check(manifest.menus.size() == 2, "the example contributes two menus");
    if (manifest.menus.size() == 2) {
        check(manifest.menus[0].action && *manifest.menus[0].action == "hello",
              "the first menu is an action invoke");
        check(manifest.menus[1].call && *manifest.menus[1].call == "version",
              "the second menu is a declarative call");
    }
    check(manifest.panels.size() == 1, "the example contributes one panel");
    if (manifest.panels.size() == 1) {
        check(manifest.panels[0].qml == "HelloPanel.qml", "the example panel qml is read");
    }
}

void test_requires_parsed_and_validated()
{
    // `requires` reads as a string array; a non-namespaced entry is refused by
    // the schema's validation.
    const ext::NativeLoadResult loaded = ext::load_native_manifest(
            data_only_manifest("dep.child", 1, { "dep.base", "dep.lib" }));
    check(loaded.manifest.has_value(), "a manifest with requires parses");
    if (loaded.manifest) {
        check(loaded.manifest->dependencies.size() == 2, "both requires are read");
        if (loaded.manifest->dependencies.size() == 2) {
            check(loaded.manifest->dependencies[0] == "dep.base", "the first require round-trips");
            check(loaded.manifest->dependencies[1] == "dep.lib", "the second require round-trips");
        }
        check(!loaded.manifest->entry.has_value(), "a data-only manifest has no entry");
    }

    const ext::NativeLoadResult bad =
            ext::load_native_manifest(data_only_manifest("dep.child", 1, { "notnamespaced" }));
    check(!bad.manifest.has_value(), "a non-namespaced require fails validation");
}

void test_dialog_parsing_and_validation()
{
    // A well-formed dialog: label, bare qml and a menu_path all round-trip.
    const ext::NativeLoadResult good = ext::load_native_manifest(
            data_only_manifest("dlg.good", 1)
            + "\n[[dialog]]\nlabel = \"Good Dialog\"\nqml = \"GoodDialog.qml\"\n"
              "menu_path = \"Tools/Good\"\n");
    check(good.manifest.has_value(), "a manifest with a dialog parses");
    if (good.manifest) {
        check(good.manifest->dialogs.size() == 1, "one dialog is read");
        if (good.manifest->dialogs.size() == 1) {
            check(good.manifest->dialogs[0].label == "Good Dialog", "the dialog label round-trips");
            check(good.manifest->dialogs[0].qml == "GoodDialog.qml", "the dialog qml round-trips");
            check(good.manifest->dialogs[0].menu_path
                          && *good.manifest->dialogs[0].menu_path == "Tools/Good",
                  "the dialog menu_path round-trips");
        }
    }

    // A dialog without a menu_path still parses, with an empty menu_path.
    const ext::NativeLoadResult no_path = ext::load_native_manifest(
            data_only_manifest("dlg.nopath", 1)
            + "\n[[dialog]]\nlabel = \"No Path\"\nqml = \"NoPath.qml\"\n");
    check(no_path.manifest.has_value(), "a dialog without a menu_path parses");
    if (no_path.manifest && no_path.manifest->dialogs.size() == 1) {
        check(!no_path.manifest->dialogs[0].menu_path.has_value(),
              "an absent menu_path reads as empty");
    }

    // A blank label fails.
    const ext::NativeLoadResult blank_label =
            ext::load_native_manifest(data_only_manifest("dlg.blank", 1)
                                      + "\n[[dialog]]\nlabel = \"\"\nqml = \"Blank.qml\"\n");
    check(!blank_label.manifest.has_value(), "a blank dialog label fails validation");

    // A qml with a path separator fails.
    const ext::NativeLoadResult bad_qml = ext::load_native_manifest(
            data_only_manifest("dlg.path", 1)
            + "\n[[dialog]]\nlabel = \"Bad Qml\"\nqml = \"sub/Bad.qml\"\n");
    check(!bad_qml.manifest.has_value(), "a dialog qml with a path separator fails");

    // A blank menu_path fails.
    const ext::NativeLoadResult blank_path = ext::load_native_manifest(
            data_only_manifest("dlg.blankpath", 1)
            + "\n[[dialog]]\nlabel = \"Blank Path\"\nqml = \"BlankPath.qml\"\nmenu_path = \"\"\n");
    check(!blank_path.manifest.has_value(), "a blank dialog menu_path fails");
}

void test_slot_parsing_and_validation()
{
    // A well-formed slot with an explicit priority round-trips.
    const ext::NativeLoadResult good = ext::load_native_manifest(
            data_only_manifest("slot.good", 1)
            + "\n[[slot]]\npoint = \"clip.inspector\"\nqml = \"InspectorSlot.qml\"\n"
              "priority = 10\n");
    check(good.manifest.has_value(), "a manifest with a slot parses");
    if (good.manifest) {
        check(good.manifest->slot_entries.size() == 1, "one slot is read");
        if (good.manifest->slot_entries.size() == 1) {
            check(good.manifest->slot_entries[0].point == "clip.inspector",
                  "the slot point round-trips");
            check(good.manifest->slot_entries[0].qml == "InspectorSlot.qml",
                  "the slot qml round-trips");
            check(good.manifest->slot_entries[0].priority == 10, "the slot priority round-trips");
        }
    }

    // An absent priority reads as the 50 default.
    const ext::NativeLoadResult no_priority = ext::load_native_manifest(
            data_only_manifest("slot.noprio", 1)
            + "\n[[slot]]\npoint = \"status.bar\"\nqml = \"StatusSlot.qml\"\n");
    check(no_priority.manifest.has_value(), "a slot without a priority parses");
    if (no_priority.manifest && no_priority.manifest->slot_entries.size() == 1) {
        check(no_priority.manifest->slot_entries[0].priority == 50,
              "an absent priority defaults to 50");
    }

    // An unknown point refuses the manifest.
    const ext::NativeLoadResult unknown_point = ext::load_native_manifest(
            data_only_manifest("slot.unknown", 1)
            + "\n[[slot]]\npoint = \"menu.tools\"\nqml = \"Tools.qml\"\n");
    check(!unknown_point.manifest.has_value(), "an unknown slot point fails validation");

    // A qml with a path separator refuses.
    const ext::NativeLoadResult bad_qml = ext::load_native_manifest(
            data_only_manifest("slot.path", 1)
            + "\n[[slot]]\npoint = \"clip.inspector\"\nqml = \"sub/Bad.qml\"\n");
    check(!bad_qml.manifest.has_value(), "a slot qml with a path separator fails");

    // An out-of-range priority refuses (both ends).
    const ext::NativeLoadResult high_priority = ext::load_native_manifest(
            data_only_manifest("slot.high", 1)
            + "\n[[slot]]\npoint = \"clip.inspector\"\nqml = \"Bad.qml\"\npriority = 101\n");
    check(!high_priority.manifest.has_value(), "a slot priority above 100 fails");
    const ext::NativeLoadResult low_priority = ext::load_native_manifest(
            data_only_manifest("slot.low", 1)
            + "\n[[slot]]\npoint = \"clip.inspector\"\nqml = \"Bad.qml\"\npriority = -1\n");
    check(!low_priority.manifest.has_value(), "a slot priority below 0 fails");
}

void test_slot_collection_and_ordering()
{
    const std::filesystem::path store = store_for("slots");
    // Two builtin data-only extensions: slot.a contributes two slots (priority
    // 10 then 5, in manifest order), slot.b one slot (priority 10). The host
    // flattens them and sorts by (priority ascending, id ascending), so the
    // tie at 10 is broken by id ("slot.a" < "slot.b").
    const std::string manifest_a = data_only_manifest("slot.a", 1)
            + "\n[[slot]]\npoint = \"status.bar\"\nqml = \"AHigh.qml\"\npriority = 10\n"
              "\n[[slot]]\npoint = \"status.bar\"\nqml = \"ALow.qml\"\npriority = 5\n";
    const std::string manifest_b = data_only_manifest("slot.b", 1)
            + "\n[[slot]]\npoint = \"status.bar\"\nqml = \"B.qml\"\npriority = 10\n";
    write_extension(store, "slot.a", 1, manifest_a, "", { }, false);
    write_extension(store, "slot.b", 1, manifest_b, "", { }, false);
    write_origins(store, "{\"slot.a\": \"builtin\", \"slot.b\": \"builtin\"}");

    CollectingHost collector;
    ext::ExtensionHost host(store, collector.build());
    host.scan();
    check(host.records().size() == 2, "both slot-bearing extensions load");

    // The per-record vectors stay in manifest order (unsorted).
    if (host.records().size() == 2) {
        check(host.records()[0].slot_entries.size() == 2, "slot.a keeps both slots");
        if (host.records()[0].slot_entries.size() == 2) {
            check(contains(host.records()[0].slot_entries[0].url, "AHigh.qml"),
                  "the record's first slot is AHigh (manifest order)");
            check(contains(host.records()[0].slot_entries[1].url, "ALow.qml"),
                  "the record's second slot is ALow (manifest order)");
        }
    }

    const std::vector<ext::SlotEntry> slots = host.slot_entries();
    check(slots.size() == 3, "three slot contributions are flattened");
    if (slots.size() == 3) {
        check(slots[0].id == "slot.a" && slots[0].priority == 5, "priority 5 sorts first");
        check(contains(slots[0].url, "ALow.qml"), "the first slot is ALow");
        check(slots[1].id == "slot.a" && slots[1].priority == 10,
              "the 10-priority slot.a sorts next");
        check(contains(slots[1].url, "AHigh.qml"), "the second slot is AHigh");
        check(slots[2].id == "slot.b" && slots[2].priority == 10,
              "the 10-priority slot.b sorts last (id tie-break)");
        check(contains(slots[2].url, "B.qml"), "the third slot is B");
        check(slots[0].point == "status.bar" && slots[1].point == "status.bar"
                      && slots[2].point == "status.bar",
              "every slot carries its point");
        check(slots[0].url.rfind("file://", 0) == 0, "the slot url is a file:// URL");
    }
}

void test_entryless_data_only_loads()
{
    const std::filesystem::path store = store_for("data_only");
    write_extension(store, "data.only", 1, data_only_manifest("data.only", 1), "", { },
                    /*with_packs=*/true);
    write_origins(store, "{\"data.only\": \"builtin\"}");

    CollectingHost collector;
    ext::ExtensionHost host(store, collector.build());
    host.scan();
    check(host.records().size() == 1, "a data-only extension loads");
    check(host.refusals().empty(), "with no refusal");
    if (host.records().size() == 1) {
        check(host.records()[0].state == ext::ExtensionState::Enabled, "and is enabled");
        check(!host.records()[0].pack_root.empty(), "and contributes its pack root");
    }
    check(host.pack_roots().size() == 1, "pack_roots lists the one root");

    const auto result = host.invoke("data.only", "greet", "{}");
    check(!result.has_value(), "invoking a data-only extension fails");
    if (!result) {
        check(contains(result.error(), "data-only"), "the failure names the data-only nature");
    }
}

void test_topological_load_order()
{
    const std::filesystem::path store = store_for("topo");
    write_extension(store, "dep.child", 1, data_only_manifest("dep.child", 1, { "dep.base" }), "",
                    { },
                    /*with_packs=*/true);
    write_extension(store, "dep.base", 1, data_only_manifest("dep.base", 1), "", { },
                    /*with_packs=*/true);
    write_origins(store, "{\"dep.child\": \"builtin\", \"dep.base\": \"builtin\"}");

    CollectingHost collector;
    ext::ExtensionHost host(store, collector.build());
    host.scan();
    check(host.records().size() == 2, "both extensions load");
    if (host.records().size() == 2) {
        check(host.records()[0].id == "dep.base", "the dependency loads before its dependent");
        check(host.records()[1].id == "dep.child", "the dependent loads second");
        check(host.records()[1].dependencies.size() == 1
                      && host.records()[1].dependencies[0] == "dep.base",
              "the dependent records its requires");
        check(host.records()[0].dependents.size() == 1
                      && host.records()[0].dependents[0] == "dep.child",
              "the dependency records its dependents");
        check(host.records()[1].dependents.empty(), "a leaf records no dependents");
        check(host.records()[0].dependencies.empty(), "the dependency records no requires");
    }
    check(host.pack_roots().size() == 2, "both pack roots are collected");
    if (host.pack_roots().size() == 2) {
        check(host.pack_roots()[0] == store / "dep.base" / "1" / "packs",
              "the dependency's pack root comes first");
        check(host.pack_roots()[1] == store / "dep.child" / "1" / "packs",
              "the dependent's pack root comes second");
    }
}

void test_cycle_fails_both_members()
{
    const std::filesystem::path store = store_for("cycle");
    write_extension(store, "cycle.a", 1, data_only_manifest("cycle.a", 1, { "cycle.b" }), "", { },
                    false);
    write_extension(store, "cycle.b", 1, data_only_manifest("cycle.b", 1, { "cycle.a" }), "", { },
                    false);
    write_origins(store, "{\"cycle.a\": \"builtin\", \"cycle.b\": \"builtin\"}");

    CollectingHost collector;
    ext::ExtensionHost host(store, collector.build());
    host.scan();
    check(host.records().empty(), "no cycle member loads");
    check(host.failed().size() == 2, "both cycle members are failed");
    if (host.failed().size() == 2) {
        check(contains(host.failed()[0].reason, "dependency cycle"),
              "the first failure names the cycle");
        check(contains(host.failed()[1].reason, "dependency cycle"),
              "the second failure names the cycle");
    }
}

void test_missing_dependency_fails()
{
    const std::filesystem::path store = store_for("missing_dep");
    write_extension(store, "dep.child", 1, data_only_manifest("dep.child", 1, { "missing.one" }),
                    "", { }, false);
    write_origins(store, "{\"dep.child\": \"builtin\"}");

    CollectingHost collector;
    ext::ExtensionHost host(store, collector.build());
    host.scan();
    check(host.records().empty(), "a missing dependency blocks the load");
    check(host.failed().size() == 1, "the dependent is failed");
    if (host.failed().size() == 1) {
        check(contains(host.failed()[0].reason, "requires missing.one"),
              "the reason names the missing dependency");
    }
}

void test_disabled_dependency_fails_dependent()
{
    const std::filesystem::path store = store_for("disabled_dep");
    write_extension(store, "dep.child", 1, data_only_manifest("dep.child", 1, { "dep.base" }), "",
                    { }, false);
    write_extension(store, "dep.base", 1, data_only_manifest("dep.base", 1), "", { }, false);
    write_origins(store, "{\"dep.child\": \"builtin\", \"dep.base\": \"builtin\"}");
    write_disabled(store, "dep.base");

    CollectingHost collector;
    ext::ExtensionHost host(store, collector.build());
    host.scan();
    check(host.records().empty(), "nothing loads when a dependency is disabled");
    check(host.disabled().size() == 1, "the disabled dependency is listed");
    if (host.disabled().size() == 1) {
        check(host.disabled()[0].id == "dep.base", "and it is dep.base");
    }
    check(host.failed().size() == 1, "the dependent is failed");
    if (host.failed().size() == 1) {
        check(contains(host.failed()[0].reason, "requires dep.base"),
              "the reason names the disabled dependency");
    }
}

void test_disabled_reason_round_trips()
{
    // The Manager's cascade writes a reason next to the id; the host must
    // surface it verbatim on the disabled record instead of dropping it.
    const std::filesystem::path store = store_for("disabled_reason");
    write_extension(store, "ext.fixture", 1, native_manifest("ext.fixture", 1), "libfixture.so",
                    fixture("ok"));
    write_origins(store, "{\"ext.fixture\": \"builtin\"}");
    write_disabled_with_reason(store, "ext.fixture", "requires dep.base");

    CollectingHost collector;
    ext::ExtensionHost host(store, collector.build());
    host.scan();
    check(host.records().empty(), "the extension is not loaded");
    check(host.disabled().size() == 1, "it is listed as disabled");
    if (host.disabled().size() == 1) {
        check(host.disabled()[0].reason == "requires dep.base",
              "the stored reason round-trips onto the record");
    }
}

void test_revoked_extension_listed_not_loaded()
{
    // A revocation forces the id off regardless of its otherwise-enabled state
    // (a consented Builtin here), and surfaces the publisher's reason.
    const std::filesystem::path store = store_for("revoked");
    write_extension(store, "ext.fixture", 1, native_manifest("ext.fixture", 1), "libfixture.so",
                    fixture("ok"));
    write_origins(store, "{\"ext.fixture\": \"builtin\"}");
    write_revoked(store, "ext.fixture", std::string("malware"));

    CollectingHost collector;
    ext::ExtensionHost host(store, collector.build());
    host.scan();
    check(host.records().empty(), "a revoked extension is not loaded");
    check(host.revoked().size() == 1, "it is listed as revoked");
    check(host.refusals().empty(), "and is not a refusal");
    if (host.revoked().size() == 1) {
        check(host.revoked()[0].state == ext::ExtensionState::Revoked, "the record is revoked");
        check(host.revoked()[0].reason == "malware", "the publisher reason round-trips");
    }
}

void test_revocation_of_other_version_still_loads()
{
    // A version-scoped revocation naming a version that is not installed does
    // not block the installed version.
    const std::filesystem::path store = store_for("revoked_other_version");
    write_extension(store, "ext.fixture", 1, native_manifest("ext.fixture", 1), "libfixture.so",
                    fixture("ok"));
    write_origins(store, "{\"ext.fixture\": \"builtin\"}");
    {
        std::ofstream out(store / "revoked.json");
        out << "{\"revocations\": [{\"id\": \"ext.fixture\", \"version\": \"2\"}]}\n";
    }

    CollectingHost collector;
    ext::ExtensionHost host(store, collector.build());
    host.scan();
    check(host.records().size() == 1, "a revocation of another version does not block the load");
    check(host.revoked().empty(), "and nothing is listed as revoked");
}

void test_override_replaces_lower_trust()
{
    const std::filesystem::path store = store_for("override_replaces");
    // A Builtin overrides a Curated's panel: the higher-trust contribution wins
    // the collision, and the loser's contribution is dropped.
    const std::string base = override_manifest(
            "cur.base", { }, "\n[[panel]]\nlabel = \"Base Panel\"\nqml = \"Base.qml\"\n");
    const std::string overrider = override_manifest(
            "bld.override", { "panel:cur.base:0" },
            "\n[[panel]]\nid = \"cur.base:0\"\nlabel = \"Override\"\nqml = \"Override.qml\"\n");
    write_extension(store, "cur.base", 1, base, "", { }, false);
    write_extension(store, "bld.override", 1, overrider, "", { }, false);
    write_origins(store, "{\"cur.base\": \"curated\", \"bld.override\": \"builtin\"}");

    CollectingHost collector;
    ext::ExtensionHost host(store, collector.build());
    host.scan();
    check(host.records().size() == 2, "both extensions load");

    bool base_lost = false;
    bool overrider_won = false;
    for (const ext::ExtensionRecord &e : host.records()) {
        if (e.id == "cur.base") {
            base_lost = e.panels.empty();
        }
        if (e.id == "bld.override") {
            overrider_won = e.panels.size() == 1 && e.panels[0].qml == "Override.qml";
        }
    }
    check(base_lost, "the Curated base's panel loses to the Builtin override");
    check(overrider_won, "the Builtin override's panel wins");

    bool saw_lose = false;
    for (const std::string &line : collector.logs) {
        if (contains(line, "cur.base") && contains(line, "loses to bld.override")) {
            saw_lose = true;
        }
    }
    check(saw_lose, "the loser is logged");
}

void test_ownership_skip_without_override()
{
    const std::filesystem::path store = store_for("ownership_skip");
    // b.free contributes a panel id owned by a.base (another extension) without
    // declaring an override: the contribution is skipped and logged.
    const std::string owner = override_manifest(
            "a.base", { }, "\n[[panel]]\nlabel = \"Owner Panel\"\nqml = \"Owner.qml\"\n");
    const std::string claimant =
            override_manifest("b.free", { },
                              "\n[[panel]]\nid = \"a.base:0\"\nlabel = \"Claimant\"\n"
                              "qml = \"Claimant.qml\"\n");
    write_extension(store, "a.base", 1, owner, "", { }, false);
    write_extension(store, "b.free", 1, claimant, "", { }, false);
    write_origins(store, "{\"a.base\": \"builtin\", \"b.free\": \"builtin\"}");

    CollectingHost collector;
    ext::ExtensionHost host(store, collector.build());
    host.scan();
    check(host.records().size() == 2, "both extensions load");

    bool owner_kept = false;
    bool claimant_dropped = false;
    for (const ext::ExtensionRecord &e : host.records()) {
        if (e.id == "a.base") {
            owner_kept = e.panels.size() == 1;
        }
        if (e.id == "b.free") {
            claimant_dropped = e.panels.empty();
        }
    }
    check(owner_kept, "the owner keeps its panel");
    check(claimant_dropped, "the claimant's panel is skipped without an override");

    bool saw_skip = false;
    for (const std::string &line : collector.logs) {
        if (contains(line, "b.free") && contains(line, "owned by a.base")
            && contains(line, "without an override; skipped")) {
            saw_skip = true;
        }
    }
    check(saw_skip, "the ownership skip is logged");
}

void test_collision_tie_break_smallest_id()
{
    const std::filesystem::path store = store_for("tie_break");
    // Two Builtin extensions both contribute a free id "free.panel"; equal trust
    // falls back to the lexicographically smaller extension id.
    const std::string aaa = override_manifest(
            "aaa.one", { },
            "\n[[panel]]\nid = \"free.panel\"\nlabel = \"AAA\"\nqml = \"Aaa.qml\"\n");
    const std::string bbb = override_manifest(
            "bbb.two", { },
            "\n[[panel]]\nid = \"free.panel\"\nlabel = \"BBB\"\nqml = \"Bbb.qml\"\n");
    write_extension(store, "aaa.one", 1, aaa, "", { }, false);
    write_extension(store, "bbb.two", 1, bbb, "", { }, false);
    write_origins(store, "{\"aaa.one\": \"builtin\", \"bbb.two\": \"builtin\"}");

    CollectingHost collector;
    ext::ExtensionHost host(store, collector.build());
    host.scan();
    check(host.records().size() == 2, "both extensions load");

    bool aaa_won = false;
    bool bbb_lost = false;
    for (const ext::ExtensionRecord &e : host.records()) {
        if (e.id == "aaa.one") {
            aaa_won = e.panels.size() == 1 && e.panels[0].qml == "Aaa.qml";
        }
        if (e.id == "bbb.two") {
            bbb_lost = e.panels.empty();
        }
    }
    check(aaa_won, "the smaller extension id wins the tie");
    check(bbb_lost, "the larger extension id loses the tie");

    bool saw_lose = false;
    for (const std::string &line : collector.logs) {
        if (contains(line, "bbb.two") && contains(line, "loses to aaa.one")) {
            saw_lose = true;
        }
    }
    check(saw_lose, "the tie loser is logged");
}

void test_unused_override_warning()
{
    const std::filesystem::path store = store_for("unused_override");
    // b.extra declares an override for a panel id it does not contribute and no
    // other extension owns: a warning, not a refusal.
    const std::string extra =
            override_manifest("b.extra", { "panel:ghost.panel" },
                              "\n[[panel]]\nlabel = \"Extra\"\nqml = \"Extra.qml\"\n");
    write_extension(store, "b.extra", 1, extra, "", { }, false);
    write_origins(store, "{\"b.extra\": \"builtin\"}");

    CollectingHost collector;
    ext::ExtensionHost host(store, collector.build());
    host.scan();
    check(host.records().size() == 1, "the extension loads despite the unused override");

    bool saw_warning = false;
    for (const std::string &line : collector.logs) {
        if (contains(line, "b.extra") && contains(line, "panel:ghost.panel")
            && contains(line, "matches nothing")) {
            saw_warning = true;
        }
    }
    check(saw_warning, "the unused override is logged as a warning");
}

void test_developer_overrides_load()
{
    const std::filesystem::path store = store_for("developer_override");
    // A Developer declaring an overrides entry now loads: Developer may
    // override (the override is a declarative/QML/asset swap, not arbitrary
    // host-code execution), and a data-only extension with no `entry` loads
    // without consent.
    const std::string dev = override_manifest("dev.override", { "panel:base.panel" },
                                              "\n[[panel]]\nlabel = \"Dev\"\nqml = \"Dev.qml\"\n");
    write_extension(store, "dev.override", 1, dev, "", { }, false);
    write_origins(store, "{\"dev.override\": \"developer\"}");

    CollectingHost collector;
    ext::ExtensionHost host(store, collector.build());
    host.scan();
    check(host.records().size() == 1, "a Developer data-only extension with overrides loads");
    check(host.failed().empty(), "nothing is failed");
    check(host.refusals().empty(), "nothing is refused");
    if (host.records().size() == 1) {
        check(host.records()[0].id == "dev.override", "the id round-trips");
        check(host.records()[0].panels.size() == 1, "the panel is collected");
    }
}

void test_developer_overrides_builtin()
{
    const std::filesystem::path store = store_for("dev_overrides_builtin");
    // A Builtin extension contributes a panel. A Developer extension declares
    // an override for that panel and contributes its own replacement panel.
    // The Developer's panel must win the collision because it explicitly
    // declares an override — the override is the declared intent to replace.
    const std::string builtin_manifest = override_manifest(
            "genesis.builtin", { }, "\n[[panel]]\nlabel = \"Builtin\"\nqml = \"Builtin.qml\"\n");
    const std::string dev_manifest =
            override_manifest("dev.override", { "panel:genesis.builtin:0" },
                              "\n[[panel]]\nid = \"genesis.builtin:0\"\nlabel = \"Override\"\n"
                              "qml = \"Override.qml\"\n");
    write_extension(store, "genesis.builtin", 1, builtin_manifest, "", { }, false);
    write_extension(store, "dev.override", 1, dev_manifest, "", { }, false);
    write_origins(store, "{\"genesis.builtin\": \"builtin\", \"dev.override\": \"developer\"}");

    CollectingHost collector;
    ext::ExtensionHost host(store, collector.build());
    host.scan();
    check(host.records().size() == 2, "both extensions load");

    bool builtin_lost = false;
    bool developer_won = false;
    for (const ext::ExtensionRecord &e : host.records()) {
        if (e.id == "genesis.builtin") {
            builtin_lost = e.panels.empty();
        }
        if (e.id == "dev.override") {
            developer_won = e.panels.size() == 1 && e.panels[0].qml == "Override.qml";
        }
    }
    check(builtin_lost, "the Builtin panel loses to the Developer override");
    check(developer_won, "the Developer's overriding panel wins");

    bool saw_lose = false;
    for (const std::string &line : collector.logs) {
        if (contains(line, "genesis.builtin") && contains(line, "loses to dev.override")) {
            saw_lose = true;
        }
    }
    check(saw_lose, "the loser is logged");
}

void test_user_overrides_refused()
{
    const std::filesystem::path store = store_for("user_override");
    // User origin is still refused for overrides — only Builtin, Curated
    // and Developer may override.
    const std::string user =
            override_manifest("user.override", { "panel:base.panel" },
                              "\n[[panel]]\nlabel = \"User\"\nqml = \"User.qml\"\n");
    write_extension(store, "user.override", 1, user, "", { }, false);
    write_origins(store, "{\"user.override\": \"user\"}");

    CollectingHost collector;
    ext::ExtensionHost host(store, collector.build());
    host.scan();
    check(host.records().empty(), "a User with overrides is refused");
    check(host.failed().size() == 1, "it is failed");
    if (host.failed().size() == 1) {
        check(contains(host.failed()[0].reason, "only Builtin, Curated and Developer"),
              "the reason names the override trust gate");
    }
}

} // namespace

int main()
{
    const std::filesystem::path dir = fixture_dir();
    if (dir.empty()) {
        std::printf("cannot resolve the fixture directory from /proc/self/exe\n");
        return 1;
    }
    std::filesystem::remove_all(kBase);
    std::filesystem::create_directories(kBase);

    test_highest_version_per_id();
    test_developer_consent_gating();
    test_bad_manifest_refused_without_log();
    test_missing_library_refused_and_logged();
    test_menus_panels_and_pack_root();
    test_curated_origin_loads_without_consent();
    test_installed_origin_drives_consent();
    test_default_origin_is_developer();
    test_builtin_disabled_is_listed_not_loaded();
    test_user_origin_is_failed();
    test_invoke_routing_and_prefixed_callbacks();
    test_shipped_example_manifest_parses();
    test_requires_parsed_and_validated();
    test_dialog_parsing_and_validation();
    test_slot_parsing_and_validation();
    test_slot_collection_and_ordering();
    test_entryless_data_only_loads();
    test_topological_load_order();
    test_cycle_fails_both_members();
    test_missing_dependency_fails();
    test_disabled_dependency_fails_dependent();
    test_disabled_reason_round_trips();
    test_revoked_extension_listed_not_loaded();
    test_revocation_of_other_version_still_loads();
    test_override_replaces_lower_trust();
    test_ownership_skip_without_override();
    test_collision_tie_break_smallest_id();
    test_unused_override_warning();
    test_developer_overrides_load();
    test_developer_overrides_builtin();
    test_user_overrides_refused();

    std::filesystem::remove_all(kBase);

    return genesis::test::summary();
}
