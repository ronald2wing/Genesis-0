// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

// The extension controller's suite: drives scan -> project -> consent grant/
// revoke -> invoke headlessly against a temp install store holding the dlopen
// fixture library, with the real api::dispatch bridge over a real Editor. No
// GStreamer and no GL: the only native code loaded is the fixture library,
// which routes a "version" call through the injected bridge.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include "app/controllers/ExtensionsController.h"
#include "app/controllers/ExtensionDialogBridge.h"
#include "extensions/Consent.h"
#include "extensions/RemovedList.h"
#include "extensions/RevokedList.h"
#include "project/Editor.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

bool contains(const std::string &haystack, const std::string &needle)
{
    return haystack.find(needle) != std::string::npos;
}

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

const std::filesystem::path kBase =
        std::filesystem::temp_directory_path() / "genesis-extensions-controller-tests";

std::filesystem::path store_for(const std::string &name)
{
    const std::filesystem::path store = kBase / name;
    std::filesystem::remove_all(store);
    std::filesystem::create_directories(store);
    return store;
}

std::string native_manifest(const std::string &id, std::uint32_t version)
{
    std::string text = "[extension]\n";
    text += "id = \"" + id + "\"\n";
    text += "name = \"Fixture\"\n";
    text += "version = " + std::to_string(version) + "\n";
    text += "api = 1\n";
    text += "entry = \"libfixture.so\"\n";
    text += "\n[[menu]]\npath = \"Tools/Fixture\"\nlabel = \"Fixture\"\n"
            "action = \"greet\"\n";
    text += "\n[[panel]]\nlabel = \"Fixture Panel\"\nqml = \"FixturePanel.qml\"\n";
    text += "\n[[dialog]]\nlabel = \"Fixture Dialog\"\nqml = \"FixtureDialog.qml\"\n"
            "menu_path = \"Tools/Fixture\"\n";
    text += "\n[[slot]]\npoint = \"clip.inspector\"\nqml = \"FixtureSlot.qml\"\n"
            "priority = 7\n";
    return text;
}

void write_extension(const std::filesystem::path &store, const std::string &id,
                     std::uint32_t version, const std::filesystem::path &library)
{
    const std::filesystem::path root = store / id / std::to_string(version);
    std::filesystem::create_directories(root);
    {
        std::ofstream out(root / "extension.toml");
        out << native_manifest(id, version);
    }
    std::filesystem::copy_file(library, root / "libfixture.so",
                               std::filesystem::copy_options::overwrite_existing);
}

void write_origins(const std::filesystem::path &store, const std::string &json_text)
{
    std::ofstream out(store / "origins.json");
    out << json_text;
}

// A data-only native manifest (no `entry`), with `requires` naming the
// extension ids this one depends on (empty for none).
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

// Writes a data-only store entry `<store>/<id>/<version>/extension.toml` (no
// library copied beside it).
void write_data_only_extension(const std::filesystem::path &store, const std::string &id,
                               std::uint32_t version,
                               const std::vector<std::string> &dependencies = { })
{
    const std::filesystem::path root = store / id / std::to_string(version);
    std::filesystem::create_directories(root);
    std::ofstream out(root / "extension.toml");
    out << data_only_manifest(id, version, dependencies);
}

void test_call_bridge()
{
    const std::filesystem::path store = store_for("call_bridge");
    genesis::project::Editor editor;
    ExtensionsController controller(&editor, store);

    // A known method routes through the same bridge the extensions use and
    // returns a JSON-RPC result envelope.
    const QString result = controller.call(QStringLiteral("version"), QStringLiteral("{}"));
    check(!result.isEmpty(), "call() returns a non-empty envelope");
    const std::string text = result.toStdString();
    check(contains(text, "\"result\""), "a known method returns a result envelope");
    check(contains(text, "apiVersion"), "and carries the version reply");

    // An unknown method returns a JSON-RPC error envelope, not nothing.
    const QString unknown = controller.call(QStringLiteral("no.such.method"), QStringLiteral("{}"));
    check(!unknown.isEmpty(), "an unknown method still returns an envelope");
    check(contains(unknown.toStdString(), "\"error\""), "and it is an error envelope");
    check(contains(unknown.toStdString(), "-32601"), "typed method-not-found");
}

void test_host_services_bridge()
{
    const std::filesystem::path store = store_for("host_services");
    genesis::project::Editor editor;
    ExtensionsController controller(&editor, store);

    // Without the app's seams a host-only method is a typed refusal; the
    // extension-store seam is still present, so extensions.list keeps working.
    const QString before = controller.call(QStringLiteral("preview.time"), QStringLiteral("{}"));
    check(contains(before.toStdString(), "-32007"),
          "preview.time without a seam is refused (NotAvailable)");

    genesis::api::Services services;
    services.preview_time = [] { return 2.5; };
    controller.setHostServices(std::move(services));

    const QString after = controller.call(QStringLiteral("preview.time"), QStringLiteral("{}"));
    check(contains(after.toStdString(), "\"result\""), "preview.time with a seam returns a result");
    check(contains(after.toStdString(), "2.5"), "and carries the injected position");

    // The extension-store seam survives the swap: extensions.list still routes
    // to the store Manager rather than falling back to a refusal.
    const QString listed = controller.call(QStringLiteral("extensions.list"), QStringLiteral("{}"));
    check(contains(listed.toStdString(), "\"result\""),
          "extensions.list still routes after setHostServices");
    check(contains(listed.toStdString(), "extensions"), "and returns the store listing");
}

void test_empty_store_lists()
{
    genesis::project::Editor editor;
    ExtensionsController controller(&editor, store_for("empty"));

    check(controller.extensions().isEmpty(), "an empty store lists no extensions");
    check(controller.menus().isEmpty(), "and no menus");
    check(controller.panels().isEmpty(), "and no panels");
    check(controller.dialogs().isEmpty(), "and no dialogs");
    check(controller.slotEntries().isEmpty(), "and no slots");
    check(controller.refusalCount() == 0, "and reports no refusals");
}

void test_scan_projection()
{
    const std::filesystem::path store = store_for("projection");
    write_extension(store, "ext.fixture", 2, fixture("ok"));
    genesis::extensions::Consent(store).grant("ext.fixture");

    genesis::project::Editor editor;
    ExtensionsController controller(&editor, store);

    check(controller.extensions().size() == 1, "one consented extension loads");
    check(controller.refusalCount() == 0, "and nothing is refused");
    if (controller.extensions().size() == 1) {
        const QVariantMap row = controller.extensions().at(0).toMap();
        check(row.value("id").toString() == QStringLiteral("ext.fixture"),
              "the id round-trips into the projection");
        check(row.value("name").toString() == QStringLiteral("Fixture"), "the name round-trips");
        check(row.value("version").toInt() == 2, "the version round-trips");
        check(row.value("origin").toString() == QStringLiteral("developer"),
              "the origin is projected");
        check(row.value("state").toString() == QStringLiteral("enabled"), "the state is projected");
        check(row.value("consented").toBool() == true, "the consent flag is projected");
    }

    check(controller.menus().size() == 1, "the one menu is projected");
    if (controller.menus().size() == 1) {
        const QVariantMap menu = controller.menus().at(0).toMap();
        check(menu.value("path").toString() == QStringLiteral("Tools/Fixture"),
              "the menu path is projected");
        check(menu.value("action").toString() == QStringLiteral("greet"),
              "the menu action is projected");
    }

    check(controller.panels().size() == 1, "the one panel is projected");
    if (controller.panels().size() == 1) {
        const QVariantMap panel = controller.panels().at(0).toMap();
        check(panel.value("qml").toString() == QStringLiteral("FixturePanel.qml"),
              "the panel qml is projected");
        check(panel.value("url").toString().startsWith(QStringLiteral("file://")),
              "the panel url is a file:// URL");
    }

    check(controller.dialogs().size() == 1, "the one dialog is projected");
    if (controller.dialogs().size() == 1) {
        const QVariantMap dialog = controller.dialogs().at(0).toMap();
        check(dialog.value("id").toString() == QStringLiteral("ext.fixture"),
              "the dialog id is projected");
        check(dialog.value("qml").toString() == QStringLiteral("FixtureDialog.qml"),
              "the dialog qml is projected");
        check(dialog.value("url").toString().startsWith(QStringLiteral("file://")),
              "the dialog url is a file:// URL");
        check(dialog.value("menu_path").toString() == QStringLiteral("Tools/Fixture"),
              "the dialog menu_path is projected");
    }

    check(controller.slotEntries().size() == 1, "the one slot is projected");
    if (controller.slotEntries().size() == 1) {
        const QVariantMap slot = controller.slotEntries().at(0).toMap();
        check(slot.value("id").toString() == QStringLiteral("ext.fixture"),
              "the slot id is projected");
        check(slot.value("point").toString() == QStringLiteral("clip.inspector"),
              "the slot point is projected");
        check(slot.value("url").toString().startsWith(QStringLiteral("file://")),
              "the slot url is a file:// URL");
        check(slot.value("url").toString().contains(QStringLiteral("FixtureSlot.qml")),
              "naming the slot's qml file");
        check(slot.value("priority").toInt() == 7, "the slot priority is projected");
    }
}

void test_slots_projection()
{
    // The host flattens every loaded extension's slot contributions and sorts
    // them by (priority ascending, id ascending). slot.a contributes two slots
    // (priority 10 then 5, in manifest order), slot.b one (priority 10), so the
    // tie at 10 is broken by id ("slot.a" < "slot.b"). Data-only extensions
    // load without a library, so the builtin origin loads them freely.
    const std::filesystem::path store = store_for("slots");

    const std::string manifest_a = data_only_manifest("slot.a", 1)
            + "\n[[slot]]\npoint = \"status.bar\"\nqml = \"AHigh.qml\"\npriority = 10\n"
              "\n[[slot]]\npoint = \"status.bar\"\nqml = \"ALow.qml\"\npriority = 5\n";
    const std::string manifest_b = data_only_manifest("slot.b", 1)
            + "\n[[slot]]\npoint = \"status.bar\"\nqml = \"B.qml\"\npriority = 10\n";
    const auto write = [&](const std::string &id, const std::string &text) {
        const std::filesystem::path root = store / id / "1";
        std::filesystem::create_directories(root);
        std::ofstream out(root / "extension.toml");
        out << text;
    };
    write("slot.a", manifest_a);
    write("slot.b", manifest_b);
    write_origins(store, "{\"slot.a\": \"builtin\", \"slot.b\": \"builtin\"}");

    genesis::project::Editor editor;
    ExtensionsController controller(&editor, store);
    check(controller.extensions().size() == 2, "both slot-bearing extensions load");

    const QVariantList entries = controller.slotEntries();
    check(entries.size() == 3, "three slot contributions are flattened");
    if (entries.size() == 3) {
        const QVariantMap first = entries.at(0).toMap();
        check(first.value("id").toString() == QStringLiteral("slot.a"), "the first slot's id");
        check(first.value("point").toString() == QStringLiteral("status.bar"),
              "the first slot's point");
        check(first.value("priority").toInt() == 5, "priority 5 sorts first");
        check(first.value("url").toString().contains(QStringLiteral("ALow.qml")),
              "the first slot is ALow");

        const QVariantMap second = entries.at(1).toMap();
        check(second.value("id").toString() == QStringLiteral("slot.a"), "the second slot's id");
        check(second.value("priority").toInt() == 10, "the 10-priority slot.a sorts next");
        check(second.value("url").toString().contains(QStringLiteral("AHigh.qml")),
              "the second slot is AHigh");

        const QVariantMap third = entries.at(2).toMap();
        check(third.value("id").toString() == QStringLiteral("slot.b"), "the third slot's id");
        check(third.value("priority").toInt() == 10, "the 10-priority slot.b sorts last");
        check(third.value("url").toString().contains(QStringLiteral("B.qml")),
              "the third slot is B");

        check(first.value("url").toString().startsWith(QStringLiteral("file://"))
                      && second.value("url").toString().startsWith(QStringLiteral("file://"))
                      && third.value("url").toString().startsWith(QStringLiteral("file://")),
              "every slot url is a file:// URL");
    }
}

void test_overrides_projection()
{
    // The host resolves collisions before the controller projects, so the
    // projection carries only the winner's panel. A Builtin overrides a
    // Curated's panel: only the Builtin's panel reaches the QML model.
    const std::filesystem::path store = store_for("overrides");

    const auto manifest = [](const std::string &id, const std::string &overrides,
                             const std::string &panel_id) {
        std::string text = "[extension]\n";
        text += "id = \"" + id + "\"\nname = \"Data " + id
                + "\"\n"
                  "version = 1\n";
        if (!overrides.empty()) {
            text += "overrides = [\"" + overrides + "\"]\n";
        }
        text += "\n[[panel]]\nid = \"" + panel_id + "\"\nlabel = \"P\"\nqml = \"" + id + ".qml\"\n";
        return text;
    };
    const auto write = [&](const std::string &id, const std::string &text) {
        const std::filesystem::path root = store / id / "1";
        std::filesystem::create_directories(root);
        std::ofstream out(root / "extension.toml");
        out << text;
    };
    write("cur.base", manifest("cur.base", "", "cur.base:0"));
    write("bld.override", manifest("bld.override", "panel:cur.base:0", "cur.base:0"));
    write_origins(store, "{\"cur.base\": \"curated\", \"bld.override\": \"builtin\"}");

    genesis::project::Editor editor;
    ExtensionsController controller(&editor, store);
    check(controller.extensions().size() == 2, "both extensions load");

    check(controller.panels().size() == 1, "only the winning panel is projected");
    if (controller.panels().size() == 1) {
        const QVariantMap panel = controller.panels().at(0).toMap();
        check(panel.value("id").toString() == QStringLiteral("bld.override"),
              "the projected panel is the Builtin override's");
        check(panel.value("qml").toString() == QStringLiteral("bld.override.qml"),
              "with the winner's qml");
    }
}

void test_invoke_routing()
{
    const std::filesystem::path store = store_for("invoke");
    write_extension(store, "ext.fixture", 1, fixture("ok"));
    genesis::extensions::Consent(store).grant("ext.fixture");

    genesis::project::Editor editor;
    ExtensionsController controller(&editor, store);

    const QVariant result = controller.invoke("ext.fixture", "greet", "{}");
    check(result.isValid(), "invoke routes to the loaded extension");
    if (result.isValid()) {
        const std::string text = result.toString().toStdString();
        check(contains(text, "greet:"), "the extension echoes the action");
        check(contains(text, "apiVersion"), "the version call reached the host bridge");
    }

    const QVariant missing = controller.invoke("ext.other", "greet", "{}");
    check(!missing.isValid(), "invoking an unknown id returns nothing");
}

void test_open_dialog_routing()
{
    const std::filesystem::path store = store_for("open_dialog");
    write_extension(store, "ext.fixture", 1, fixture("ok"));
    genesis::extensions::Consent(store).grant("ext.fixture");

    genesis::project::Editor editor;
    ExtensionsController controller(&editor, store);
    check(controller.dialogs().size() == 1, "the consented extension contributes one dialog");

    // The bridge is always available, and close() raises closeRequested.
    ExtensionDialogBridge *bridge = controller.dialogBridge();
    check(bridge != nullptr, "dialogBridge() returns a bridge");
    if (bridge != nullptr) {
        int closed = 0;
        QObject::connect(bridge, &ExtensionDialogBridge::closeRequested, [&] { ++closed; });
        bridge->close();
        check(closed == 1, "close() raises closeRequested once");
    }

    // openDialog routes the matching id through dialogRequested, carrying the
    // dialog's projected file URL.
    int requested = 0;
    QString requested_id;
    QString requested_url;
    QObject::connect(&controller, &ExtensionsController::dialogRequested,
                     [&](const QString &id, const QString &url) {
                         ++requested;
                         requested_id = id;
                         requested_url = url;
                     });
    controller.openDialog(QStringLiteral("ext.fixture"));
    check(requested == 1, "openDialog emits dialogRequested once");
    if (requested == 1) {
        check(requested_id == QStringLiteral("ext.fixture"), "with the extension id");
        check(requested_url.startsWith(QStringLiteral("file://")),
              "with a file:// URL for the dialog");
        check(requested_url.contains(QStringLiteral("FixtureDialog.qml")),
              "naming the dialog's qml file");
    }

    // An unknown id is a no-op: no signal.
    controller.openDialog(QStringLiteral("ext.unknown"));
    check(requested == 1, "an unknown dialog id emits nothing");
}

void test_consent_grant_and_revoke()
{
    const std::filesystem::path store = store_for("consent");
    write_extension(store, "ext.fixture", 1, fixture("ok"));

    genesis::project::Editor editor;
    ExtensionsController controller(&editor, store);
    // No consent: the extension is still listed, but failed.
    check(controller.extensions().size() == 1, "no consent still lists the extension");
    check(controller.extensions().at(0).toMap().value("state").toString()
                  == QStringLiteral("failed"),
          "and it is listed as failed");
    check(controller.refusalCount() == 1, "the unconsented id is refused");

    int changed = 0;
    QObject::connect(&controller, &ExtensionsController::extensionsChanged, [&] { ++changed; });

    controller.grantConsent(QStringLiteral("ext.fixture"));
    check(controller.extensions().size() == 1, "granting consent loads it");
    check(controller.extensions().at(0).toMap().value("state").toString()
                  == QStringLiteral("enabled"),
          "and it is listed as enabled");
    check(controller.refusalCount() == 0, "and the refusal clears");
    check(changed == 1, "grant emits extensionsChanged once");

    controller.revokeConsent(QStringLiteral("ext.fixture"));
    check(controller.extensions().size() == 1, "revoking consent keeps it listed");
    check(controller.extensions().at(0).toMap().value("state").toString()
                  == QStringLiteral("failed"),
          "but listed as failed");
    check(controller.refusalCount() == 1, "and the refusal returns");
    check(changed == 2, "revoke emits extensionsChanged again");
}

void test_set_enabled_routes_by_origin()
{
    // A Developer extension toggles consent; a Curated one toggles the disabled
    // list. Both go through the one setEnabled switch.
    const std::filesystem::path store = store_for("set_enabled");
    write_extension(store, "ext.fixture", 1, fixture("ok"));

    genesis::project::Editor editor;
    ExtensionsController controller(&editor, store);

    controller.setEnabled(QStringLiteral("ext.fixture"), true);
    check(controller.extensions().at(0).toMap().value("state").toString()
                  == QStringLiteral("enabled"),
          "setEnabled(true) grants Developer consent and loads");
    check(controller.refusalCount() == 0, "and nothing is refused");

    controller.setEnabled(QStringLiteral("ext.fixture"), false);
    check(controller.extensions().at(0).toMap().value("state").toString()
                  == QStringLiteral("failed"),
          "setEnabled(false) revokes consent and lists it failed");
    check(controller.refusalCount() == 1, "and the refusal returns");

    // The same id as Curated: disable is not a consent question, it is the
    // disabled list, so no refusal is ever reported.
    const std::filesystem::path curated_store = store_for("set_enabled_curated");
    write_extension(curated_store, "ext.fixture", 1, fixture("ok"));
    write_origins(curated_store, "{\"ext.fixture\": \"curated\"}");

    ExtensionsController curated(&editor, curated_store);
    check(curated.extensions().at(0).toMap().value("state").toString() == QStringLiteral("enabled"),
          "a Curated extension starts enabled");

    curated.setEnabled(QStringLiteral("ext.fixture"), false);
    check(curated.extensions().at(0).toMap().value("state").toString()
                  == QStringLiteral("disabled"),
          "setEnabled(false) disables a Curated extension");
    check(curated.refusalCount() == 0, "and a disable is never a refusal");

    curated.setEnabled(QStringLiteral("ext.fixture"), true);
    check(curated.extensions().at(0).toMap().value("state").toString() == QStringLiteral("enabled"),
          "setEnabled(true) re-enables it");
}

void test_projection_requires_dependents()
{
    const std::filesystem::path store = store_for("projection_deps");
    write_data_only_extension(store, "dep.child", 1, { "dep.base" });
    write_data_only_extension(store, "dep.base", 1);
    write_origins(store, "{\"dep.child\": \"builtin\", \"dep.base\": \"builtin\"}");

    genesis::project::Editor editor;
    ExtensionsController controller(&editor, store);

    check(controller.extensions().size() == 2, "both data-only extensions load");
    if (controller.extensions().size() == 2) {
        const QVariantMap base = controller.extensions().at(0).toMap();
        const QVariantMap child = controller.extensions().at(1).toMap();
        check(base.value("id").toString() == QStringLiteral("dep.base"),
              "the dependency loads first");
        check(base.value("dependents").toStringList() == QStringList{ QStringLiteral("dep.child") },
              "the dependency's dependents are projected");
        check(child.value("requires").toStringList() == QStringList{ QStringLiteral("dep.base") },
              "the dependent's requires are projected");
        check(base.value("requires").toStringList().isEmpty(),
              "the dependency projects no requires");
        check(child.value("dependents").toStringList().isEmpty(),
              "the leaf projects no dependents");
    }
}

void test_remove_and_restore_invokables()
{
    const std::filesystem::path store = store_for("remove");
    write_extension(store, "ext.curated", 1, fixture("ok"));
    write_origins(store, "{\"ext.curated\": \"curated\"}");

    genesis::project::Editor editor;
    ExtensionsController controller(&editor, store);
    check(controller.extensions().size() == 1, "the curated extension loads");

    check(controller.removeExtension(QStringLiteral("ext.curated")) == true,
          "removeExtension succeeds for a curated id");
    check(controller.extensions().isEmpty(), "the removed extension leaves the projection");
    check(!std::filesystem::exists(store / "ext.curated"), "and its installed copy is deleted");

    check(controller.restoreExtension(QStringLiteral("ext.curated")) == true,
          "restoreExtension clears the tombstone");

    // A builtin is removable by the user: remove tombstones + deletes it and
    // lists it under removedBuiltins; restore clears the tombstone so the
    // startup seed can re-add it.
    const std::filesystem::path builtin_store = store_for("remove_builtin");
    write_extension(builtin_store, "ext.builtin", 1, fixture("ok"));
    write_origins(builtin_store, "{\"ext.builtin\": \"builtin\"}");

    ExtensionsController builtin(&editor, builtin_store);
    check(builtin.removeExtension(QStringLiteral("ext.builtin")) == true,
          "removeExtension tombstones and deletes a builtin id");
    check(builtin.extensions().isEmpty(), "and the builtin leaves the projection");
    check(!std::filesystem::exists(builtin_store / "ext.builtin"),
          "and its installed copy is deleted");
    check(builtin.removedBuiltins().size() == 1, "and it is listed under removed builtins");
    if (builtin.removedBuiltins().size() == 1) {
        check(builtin.removedBuiltins().at(0).toMap().value("id").toString()
                      == QStringLiteral("ext.builtin"),
              "with its id projected");
    }

    check(builtin.restoreExtension(QStringLiteral("ext.builtin")) == true,
          "restoreExtension clears a builtin's tombstone");
    check(builtin.removedBuiltins().isEmpty(), "and it leaves the removed-builtins list");
    check(!genesis::extensions::removed(builtin_store, "ext.builtin"),
          "and the tombstone record is cleared");
}

void test_removed_builtins_projection()
{
    // The projection is the intersection of removed.json with the builtin ids
    // (recorded in origins.json): a tombstoned builtin is listed, a tombstoned
    // Curated id is not.
    const std::filesystem::path store = store_for("removed_projection");
    write_extension(store, "ext.builtin", 1, fixture("ok"));
    write_extension(store, "ext.curated", 1, fixture("ok"));
    write_origins(store, "{\"ext.builtin\": \"builtin\", \"ext.curated\": \"curated\"}");

    genesis::project::Editor editor;
    ExtensionsController controller(&editor, store);
    check(controller.removedBuiltins().isEmpty(), "no tombstones projects no removed builtins");

    // Tombstone both ids directly (the record the CLI remove verb writes).
    check(genesis::extensions::remove(store, "ext.builtin"), "tombstoning the builtin succeeds");
    check(genesis::extensions::remove(store, "ext.curated"), "tombstoning the curated id succeeds");
    controller.refresh();

    check(controller.removedBuiltins().size() == 1, "only the tombstoned builtin is projected");
    if (controller.removedBuiltins().size() == 1) {
        check(controller.removedBuiltins().at(0).toMap().value("id").toString()
                      == QStringLiteral("ext.builtin"),
              "and it names the builtin id");
    }
}

// Writes a source directory (not a store entry) holding a native manifest and
// the fixture library, the shape `install_extension` expects.
std::filesystem::path write_source_extension(const std::string &name, const std::string &id,
                                             std::uint32_t version,
                                             const std::filesystem::path &library)
{
    const std::filesystem::path root = kBase / "sources" / name;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    {
        std::ofstream out(root / "extension.toml");
        out << native_manifest(id, version);
    }
    std::filesystem::copy_file(library, root / "libfixture.so",
                               std::filesystem::copy_options::overwrite_existing);
    return root;
}

void test_install_from_directory()
{
    const std::filesystem::path store = store_for("install");
    const std::filesystem::path source =
            write_source_extension("good", "ext.fixture", 3, fixture("ok"));

    genesis::project::Editor editor;
    ExtensionsController controller(&editor, store);
    check(controller.extensions().isEmpty(), "the store starts empty");

    int changed = 0;
    QObject::connect(&controller, &ExtensionsController::extensionsChanged, [&] { ++changed; });

    const QString result = controller.install(QString::fromStdString(source.string()));
    check(result == QStringLiteral("installed ext.fixture v3"),
          "install returns the installed id and version");
    check(changed == 1, "install re-scans and emits extensionsChanged once");

    // The Developer install lands consent-gated: the row is present but failed
    // with a consent reason, and the store holds the copied directory.
    check(controller.extensions().size() == 1, "the installed extension appears in the projection");
    if (controller.extensions().size() == 1) {
        const QVariantMap row = controller.extensions().at(0).toMap();
        check(row.value("id").toString() == QStringLiteral("ext.fixture"), "with the manifest id");
        check(row.value("origin").toString() == QStringLiteral("developer"),
              "as a Developer origin");
        check(row.value("state").toString() == QStringLiteral("failed"),
              "consent-gated, so listed failed");
        check(row.value("consented").toBool() == false, "and not yet consented");
        check(!row.value("reason").toString().isEmpty(),
              "with a reason naming the missing consent");
    }
    check(controller.refusalCount() == 1, "the unconsented install is refused");
    check(std::filesystem::exists(store / "ext.fixture" / "3" / "extension.toml"),
          "the source directory is copied into the store");

    // The Consent toggle is the enable path: granting loads the row.
    controller.grantConsent(QStringLiteral("ext.fixture"));
    check(controller.extensions().at(0).toMap().value("state").toString()
                  == QStringLiteral("enabled"),
          "granting consent enables the installed extension");
    check(controller.refusalCount() == 0, "and the refusal clears");
}

void test_install_bad_directory_refused()
{
    const std::filesystem::path store = store_for("install_bad");
    // A directory with no manifest at all: install must refuse without writing.
    const std::filesystem::path source = kBase / "sources" / "empty";
    std::filesystem::remove_all(source);
    std::filesystem::create_directories(source);

    genesis::project::Editor editor;
    ExtensionsController controller(&editor, store);

    const QString result = controller.install(QString::fromStdString(source.string()));
    check(result.startsWith(QStringLiteral("error:")),
          "a directory with no manifest returns an error string");
    check(controller.extensions().isEmpty(), "and nothing appears in the projection");
    check(!std::filesystem::exists(store / "ext.fixture"), "and the store is unchanged");

    // An unknown origin is refused before any filesystem work.
    const std::filesystem::path good =
            write_source_extension("good2", "ext.fixture", 1, fixture("ok"));
    const QString bad_origin =
            controller.install(QString::fromStdString(good.string()), QStringLiteral("nonsense"));
    check(bad_origin.startsWith(QStringLiteral("error:")),
          "an unknown origin returns an error string");
    check(controller.extensions().isEmpty(), "and an unknown origin writes nothing");
}

// Builds a catalog document listing one `dir`-source entry, with an optional
// `revoked` array appended. The source directory need not exist for a
// refused-install test: the revocation gate runs before any source work.
std::string catalog_json(const std::string &id, const std::string &source,
                         const std::string &revoked = "")
{
    std::string text = "{\"entries\": [{\"id\": \"" + id
            + "\", \"name\": \"Catalog\", \"version\": \"1\", \"description\": \"\", "
              "\"source\": {\"type\": \"dir\", \"url\": \""
            + source + "\"}}]";
    if (!revoked.empty()) {
        text += ", \"revoked\": [" + revoked + "]";
    }
    text += "}";
    return text;
}

void test_install_from_catalog()
{
    const std::filesystem::path store = store_for("install_catalog");
    const std::filesystem::path source =
            write_source_extension("catalog_good", "ext.catalog", 4, fixture("ok"));
    const std::string catalog = catalog_json("ext.catalog", source.string());

    genesis::project::Editor editor;
    ExtensionsController controller(&editor, store);
    controller.setCatalogFetcher(
            [&](const std::string &) -> std::optional<std::string> { return catalog; });

    int changed = 0;
    QObject::connect(&controller, &ExtensionsController::extensionsChanged, [&] { ++changed; });

    const QString result = controller.installFromCatalog(
            QStringLiteral("https://c.example/genesis.json"), QStringLiteral("ext.catalog"));
    check(result == QStringLiteral("installed ext.catalog v4"),
          "installFromCatalog returns the installed id and version");
    check(changed == 1, "installFromCatalog re-scans and emits extensionsChanged once");

    // A Developer install lands consent-gated: the row is present but failed.
    check(controller.extensions().size() == 1,
          "the catalog-installed extension appears in the projection");
    if (controller.extensions().size() == 1) {
        const QVariantMap row = controller.extensions().at(0).toMap();
        check(row.value("id").toString() == QStringLiteral("ext.catalog"), "with the manifest id");
        check(row.value("state").toString() == QStringLiteral("failed"),
              "consent-gated, so listed failed");
    }
    check(std::filesystem::exists(store / "ext.catalog" / "4" / "extension.toml"),
          "the catalog source is copied into the store");
}

void test_install_from_catalog_refuses_revoked_entry()
{
    const std::filesystem::path store = store_for("install_catalog_revoked");
    // The source path need not exist: the revocation gate runs first.
    const std::string catalog = catalog_json("ext.revoked", "/tmp/does-not-exist",
                                             "{\"id\": \"ext.revoked\", \"reason\": \"malware\"}");

    genesis::project::Editor editor;
    ExtensionsController controller(&editor, store);
    controller.setCatalogFetcher(
            [&](const std::string &) -> std::optional<std::string> { return catalog; });

    const QString result = controller.installFromCatalog(
            QStringLiteral("https://c.example/genesis.json"), QStringLiteral("ext.revoked"));
    check(result.startsWith(QStringLiteral("error:")), "a revoked entry is refused");
    check(result.contains(QStringLiteral("malware")), "with the publisher's reason");
    check(controller.extensions().isEmpty(), "and nothing is installed");
}

void test_projection_lists_revoked()
{
    const std::filesystem::path store = store_for("projection_revoked");
    write_extension(store, "ext.fixture", 1, fixture("ok"));
    genesis::extensions::Consent(store).grant("ext.fixture");

    genesis::project::Editor editor;
    ExtensionsController controller(&editor, store);
    check(controller.extensions().at(0).toMap().value("state").toString()
                  == QStringLiteral("enabled"),
          "the consented extension starts enabled");

    // Persist a version-less revocation, then re-scan: the row is forced off
    // and listed revoked with the publisher's reason.
    check(genesis::extensions::replace_revocations(
                  store,
                  { genesis::extensions::Revocation{ "ext.fixture", std::nullopt,
                                                     std::string("malware") } }),
          "the revocation is persisted");
    controller.refresh();

    check(controller.extensions().size() == 1, "the revoked extension is still listed");
    if (controller.extensions().size() == 1) {
        const QVariantMap row = controller.extensions().at(0).toMap();
        check(row.value("state").toString() == QStringLiteral("revoked"),
              "and its state projects as revoked");
        check(row.value("reason").toString() == QStringLiteral("malware"),
              "carrying the publisher's reason");
    }
}

void test_install_via_bridge()
{
    const std::filesystem::path store = store_for("install_bridge");
    const std::filesystem::path source =
            write_source_extension("bridge", "ext.bridge", 2, fixture("ok"));

    genesis::project::Editor editor;
    ExtensionsController controller(&editor, store);
    check(controller.extensions().isEmpty(), "the store starts empty");

    // A string `source` installs through the same bridge the extensions reach.
    const std::string params = "{\"source\": \"" + source.string() + "\"}";
    const QString result =
            controller.call(QStringLiteral("extensions.install"), QString::fromStdString(params));
    const std::string text = result.toStdString();
    check(contains(text, "\"result\""),
          "extensions.install via the bridge returns a result envelope");
    check(contains(text, "installed"), "and carries the installed path");
    check(std::filesystem::exists(store / "ext.bridge" / "2" / "extension.toml"),
          "the source directory is copied into the store");

    // The Manager seam shares the controller's store, so a refresh adopts the
    // bridge-installed extension into the projection.
    controller.refresh();
    check(controller.extensions().size() == 1, "a refresh adopts the bridge-installed extension");

    // The object source form decodes and installs too.
    const std::filesystem::path obj_source =
            write_source_extension("bridge_obj", "ext.bridgeobj", 1, fixture("ok"));
    const std::string obj_params =
            "{\"source\": {\"type\": \"dir\", \"url\": \"" + obj_source.string() + "\"}}";
    const QString obj_result = controller.call(QStringLiteral("extensions.install"),
                                               QString::fromStdString(obj_params));
    check(contains(obj_result.toStdString(), "\"result\""),
          "an object dir source installs through the bridge");
    check(std::filesystem::exists(store / "ext.bridgeobj" / "1" / "extension.toml"),
          "the object-source install lands in the store");

    // An unknown origin is refused at the codec, before any store work.
    const std::string bad_params =
            "{\"source\": \"" + source.string() + "\", \"origin\": \"nonsense\"}";
    const QString bad = controller.call(QStringLiteral("extensions.install"),
                                        QString::fromStdString(bad_params));
    check(contains(bad.toStdString(), "\"error\""), "an unknown origin returns an error envelope");
    check(contains(bad.toStdString(), "-32600"), "typed invalid-params");
}

void test_refresh_picks_up_new_installs()
{
    const std::filesystem::path store = store_for("refresh");
    genesis::project::Editor editor;
    ExtensionsController controller(&editor, store);
    check(controller.extensions().isEmpty(), "the store starts empty");

    // An extension lands on disk after the controller was constructed; the
    // explicit refresh re-scans and adopts it.
    write_extension(store, "ext.fixture", 1, fixture("ok"));
    genesis::extensions::Consent(store).grant("ext.fixture");
    controller.refresh();
    check(controller.extensions().size() == 1, "refresh adopts the new install");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const std::filesystem::path dir = fixture_dir();
    if (dir.empty()) {
        std::printf("cannot resolve the fixture directory from /proc/self/exe\n");
        return 1;
    }
    std::filesystem::remove_all(kBase);
    std::filesystem::create_directories(kBase);

    test_empty_store_lists();
    test_call_bridge();
    test_host_services_bridge();
    test_scan_projection();
    test_slots_projection();
    test_overrides_projection();
    test_invoke_routing();
    test_open_dialog_routing();
    test_consent_grant_and_revoke();
    test_set_enabled_routes_by_origin();
    test_projection_requires_dependents();
    test_remove_and_restore_invokables();
    test_removed_builtins_projection();
    test_install_from_directory();
    test_install_bad_directory_refused();
    test_install_from_catalog();
    test_install_from_catalog_refuses_revoked_entry();
    test_projection_lists_revoked();
    test_install_via_bridge();
    test_refresh_picks_up_new_installs();

    std::filesystem::remove_all(kBase);

    return genesis::test::summary();
}
