// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The studio shell's extension controller: the thin layer that owns the
// extension host (which scans the installed-extension store and loads every
// loadable native extension) and exposes the loaded extensions, their menu
// actions and their QML panels to QML. A menu action invokes its extension
// through the host; the extension reaches the host API through the injected
// call_json bridge, which routes to api::dispatch over the one open Editor, so
// a mutating action is an undoable edit the shell refreshes exactly like any
// other. It owns no QML.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "api/Api.h"

namespace genesis::extensions {
class ExtensionHost;
struct ExtensionRecord;
class Manager;
} // namespace genesis::extensions

namespace genesis::project {
class Editor;
} // namespace genesis::project

class ExtensionDialogBridge;

class ExtensionsController : public QObject
{
    Q_OBJECT

    // The installed native extensions, one map per extension, in this order:
    // enabled, then disabled, then revoked, then failed:
    //   { "id": QString, "name": QString, "version": int,
    //     "origin": QString, "state": QString, "reason": QString,
    //     "requires": QStringList, "dependents": QStringList,
    //     "consented": bool }.
    // `origin` is one of "builtin"/"curated"/"developer"/"user"; `state` one of
    // "enabled"/"disabled"/"revoked"/"failed"; `reason` is why a non-enabled
    // extension is not enabled (empty otherwise); `requires`/`dependents` are
    // the ids this extension depends on / that depend on it; `consented` records
    // Developer consent.
    Q_PROPERTY(QVariantList extensions READ extensions NOTIFY extensionsChanged)
    // The menu entries every loaded extension contributed, one map per entry:
    //   { "id": QString, "path": QString, "label": QString,
    //     "action": QString, "call": QString }.
    // `id` is the extension's id (the invoke target); exactly one of `action`
    // and `call` is non-empty.
    Q_PROPERTY(QVariantList menus READ menus NOTIFY extensionsChanged)
    // The QML panels every loaded extension contributed, one map per panel:
    //   { "id": QString, "label": QString, "qml": QString,
    //     "url": QString }.
    // `url` is the `file://` URL the QML loader opens.
    Q_PROPERTY(QVariantList panels READ panels NOTIFY extensionsChanged)
    // The dialogs every loaded extension contributed, one map per dialog:
    //   { "id": QString, "label": QString, "qml": QString,
    //     "url": QString, "menu_path": QString }.
    // `id` is the extension's id (the openDialog target); `url` is the `file://`
    // URL the QML loader opens; `menu_path` is the optional menu path the
    // dialog's menu entry lives under (empty -> the Extensions menu).
    Q_PROPERTY(QVariantList dialogs READ dialogs NOTIFY extensionsChanged)
    // The slots every loaded extension contributed, one map per slot, ordered
    // by priority (ascending) then extension id:
    //   { "id": QString, "point": QString, "url": QString, "priority": int }.
    // `point` names the host surface the slot embeds into
    // (`clip.inspector`/`status.bar`/`settings.updates`); `url` is the `file://`
    // URL the per-contribution Loader opens.
    // `slots` is also a Qt keyword macro; undefine it for the property's name
    // only, then restore it for anything included after this header.
#undef slots
    Q_PROPERTY(QVariantList slots READ slotEntries NOTIFY extensionsChanged)
#define slots Q_SLOTS
    // How many installed native extensions were refused to load (untrusted, no
    // consent, or a bad manifest/library), reported rather than silently
    // dropped.
    Q_PROPERTY(int refusalCount READ refusalCount NOTIFY extensionsChanged)
    // The tombstoned builtin extensions, one map per id: { "id": QString }.
    // The Removed section's Restore buttons. Derived from the store's
    // removed.json intersected with the builtin ids (whose origins.json record
    // is "builtin"); a removed builtin's origin record persists (removal
    // deletes the installed copy, not the record), so the intersection is
    // exactly the builtins the user removed.
    Q_PROPERTY(QVariantList removedBuiltins READ removedBuiltins NOTIFY extensionsChanged)
    // Undo/redo/dirty, mirrored from the host Editor so the toolbar stays in
    // step with extension-driven edits.
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY historyChanged)

public:
    // `editor` is the one open project, which the call_json bridge drives.
    // `store_root` is the installed-extension store (AppDataLocation/
    // "extensions"). The store is scanned once here; call refresh() to re-scan.
    ExtensionsController(genesis::project::Editor *editor, std::filesystem::path store_root,
                         QObject *parent = nullptr);

    // Defined out of line: host_ holds a ExtensionHost behind a forward
    // declaration, so the destructor must be instantiated where that type is
    // complete (the .cpp), not in every TU that reads this header.
    ~ExtensionsController() override;

    QVariantList extensions() const { return extensions_; }
    QVariantList menus() const { return menus_; }
    QVariantList panels() const { return panels_; }
    QVariantList dialogs() const { return dialogs_; }
    QVariantList slotEntries() const { return slotEntries_; }
    int refusalCount() const { return refusalCount_; }
    QVariantList removedBuiltins() const { return removedBuiltins_; }

    // The bridge every contributed dialog receives as `dialogBridge`: its
    // dismiss affordance. The shell exposes the same object as a root context
    // property, so a dialog calls dialogBridge.close() and the overlay host
    // tears itself down.
    ExtensionDialogBridge *dialogBridge() const;

    bool canUndo() const;
    bool canRedo() const;
    bool dirty() const;

    // The contributed pack directories the host collected, for the shell's
    // effect catalogue (built once over these roots plus the builtin source).
    const std::vector<std::filesystem::path> &pack_roots() const { return pack_roots_; }

    // The loaded content records from the ExtensionHost, for building the effect
    // catalogue directly from parsed [[effect]] blocks.
    const std::vector<genesis::extensions::ExtensionRecord> &records() const;

    // Re-scans the store and re-derives the projection. Called from the
    // constructor; the store is static on disk, so there is no per-edit refresh.
    // Q_INVOKABLE so the dialog can re-scan after a catalog install, which
    // routes through the API's Manager seam (not this controller) and so does
    // not refresh the projection on its own.
    Q_INVOKABLE void refresh();

    // Reads `path` (a local path or file:// URL) as UTF-8 text, or returns an
    // empty string when it cannot be read. The config-profile section needs
    // this because the `config.export`/`config.import` bridge verbs carry the
    // profile inline, not by path: the dialog writes the exported document and
    // reads the imported one itself.
    Q_INVOKABLE QString readTextFile(const QString &path) const;

    // Writes `text` to `path` (a local path or file:// URL) as UTF-8, returning
    // an empty string on success or a human-readable reason on failure. The
    // write is atomic (QSaveFile), so a failed export leaves no partial file.
    Q_INVOKABLE QString writeTextFile(const QString &path, const QString &text) const;

    // Invokes `action` on the extension named `id`, forwarding `args` (a JSON
    // object string) and returning the JSON result the extension produced, or
    // an empty variant on failure. A mutating action applies through the one
    // Editor, so the shell refreshes exactly like any other edit.
    Q_INVOKABLE QVariant invoke(const QString &id, const QString &action, const QString &args);

    // Routes one host API call through the same dispatch/bridge the loaded
    // extensions use, returning the JSON-RPC reply envelope as a string. This
    // is how an extension's QML panel invokes host methods (template.list,
    // template.fill, edit.apply, ...). Panels carry their extension's trust:
    // the bridge does not re-check the panel's origin, so whatever the loaded
    // extension was granted to do, its panels can do. `paramsJson` is a JSON
    // object (or empty); an unknown method, or a method whose params fail to
    // decode, returns an error envelope rather than nothing.
    Q_INVOKABLE QString call(const QString &method, const QString &paramsJson);

    // Opens the first dialog the extension `id` contributed, routing to the
    // shell's overlay host through the `dialogRequested` signal. An unknown id
    // (or an id with no dialogs) is a no-op. The dialog's own dismissal comes
    // through dialogBridge().close(), not here.
    Q_INVOKABLE void openDialog(const QString &id);

    // Records consent for the Developer-origin native extension `id` and
    // re-scans, so the extension becomes loadable. This is the shell's answer
    // to a refusal that is consent-gated.
    Q_INVOKABLE void grantConsent(const QString &id);

    // Withdraws consent for `id` and re-scans, unloading it from the shell's
    // projection. Consent is the gate that keeps a local-only extension out of
    // the process, so revoking it is the enable/disable switch.
    Q_INVOKABLE void revokeConsent(const QString &id);

    // The single enable/disable switch, keyed by origin: a Developer extension
    // is consent-gated (enabled == grant, disabled == revoke); a Builtin/Curated
    // extension is toggled on the store's disabled list. Either way the store is
    // updated and the projection re-scanned.
    Q_INVOKABLE void setEnabled(const QString &id, bool enabled);

    // Removes / restores the installed native extension `id`. A Builtin id is
    // tombstoned (so the startup seed never re-adds it), its installed copy
    // deleted and its dependents disabled; restoring clears the tombstone and
    // re-seeds the builtins, so the id reappears. A Curated/Developer id routes
    // through the store Manager (tombstone/un-tombstone). Removal disables the
    // id's dependents either way. Returns true when the store changed.
    Q_INVOKABLE bool removeExtension(const QString &id);
    Q_INVOKABLE bool restoreExtension(const QString &id);

    // Installs the extension directory `sourceDir` into the store, under the id
    // and version its manifest declares. `originName` is one of
    // "builtin"/"curated"/"developer"/"user" (default "developer"); an unknown
    // name is refused. The kind (native vs Pack) is detected from the manifest
    // present. On success the store is re-scanned, so a Developer install
    // appears immediately as a consent-gated row. Returns a compact result
    // string: "installed <id> v<version>" on success, else "error: <reason>"
    // (the first problem, or the unknown-origin message). Nothing is written to
    // the store when the install is refused.
    Q_INVOKABLE QString install(const QString &sourceDir,
                                const QString &originName = QStringLiteral("developer"));

    // Installs the entry named `id` from the marketplace catalog at `url`, the
    // app-side counterpart to the CLI's `install --catalog`. The fetch goes
    // through the injected `catalog_fetcher_` (see setCatalogFetcher), so a
    // panel never reaches the network itself; the origin rules, the entry's
    // integrity fields and the catalog's revocation list are all enforced by
    // the shared install_from_catalog path, and the catalog's `revoked` list is
    // reconciled into the store so a later scan forces any revoked extension
    // off. Returns the same compact result as install(): "installed <id>
    // v<version>" on success, else "error: <reason>" (no fetcher wired, the
    // fetch/parse failed, the id is unknown, or the entry is revoked). A
    // success re-scans, so the new row appears immediately.
    Q_INVOKABLE QString installFromCatalog(const QString &url, const QString &id,
                                           const QString &originName = QStringLiteral("developer"));

    // Injects the marketplace-catalog fetch (a URL -> body function) the
    // installFromCatalog invokable uses. An empty function (the default) means
    // "no fetcher wired": installFromCatalog refuses rather than reaching the
    // network. The fetcher type is spelled out (a using-alias cannot be
    // forward-declared); it matches extensions::CatalogFetcher.
    void setCatalogFetcher(std::function<std::optional<std::string>(const std::string &)> fetcher);

    // Installs the app's host services into the API bridge, so a panel's calls
    // to the app-owned verbs (export.run, scopes.snapshot, edit.selection,
    // preview.time, media.list, gpu.status) reach the real controllers. The
    // extension-store seam set in the constructor is preserved (it holds the
    // store Manager); everything else is replaced. Called by the composition
    // root once every controller exists.
    void setHostServices(genesis::api::Services services);

signals:
    // extensions / menus / panels / refusalCount changed.
    void extensionsChanged();
    // canUndo / canRedo / dirty changed.
    void historyChanged();
    // The project changed (an extension action mutated it): the shell rebuilds
    // its projection and reloads the engine.
    void projectEdited();
    // A contributed dialog asks the shell to open its overlay. `id` is the
    // extension id (the openDialog target), `url` the absolute file URL the
    // overlay's Loader opens.
    void dialogRequested(const QString &id, const QString &url);

private:
    // The call_json bridge handed to every loaded extension: routes a method to
    // api::dispatch over the one Editor, serialising the reply as a JSON-RPC
    // envelope. Always produces a reply (0), even an error envelope.
    int callJson(const std::string &method, const std::string &params, std::string &out);

    genesis::project::Editor *editor_;
    // The store root, retained so grantConsent/revokeConsent can write the
    // consent list after the host took its own copy.
    std::filesystem::path store_root_;
    genesis::api::Services services_;
    // The store Manager, shared with the API seam so enable/disable/remove/
    // restore reach the same store the host scans.
    std::shared_ptr<genesis::extensions::Manager> manager_;
    std::unique_ptr<genesis::extensions::ExtensionHost> host_;
    // The marketplace-catalog fetch installFromCatalog uses; empty means "no
    // fetcher wired". Injected by the composition root (the only place the
    // HTTP client lives), so the controller owns no network code.
    std::function<std::optional<std::string>(const std::string &)> catalog_fetcher_;
    QVariantList extensions_;
    QVariantList menus_;
    QVariantList panels_;
    QVariantList dialogs_;
    QVariantList slotEntries_;
    QVariantList removedBuiltins_;
    int refusalCount_ = 0;
    std::vector<std::filesystem::path> pack_roots_;
    // The bridge handed to every contributed dialog (and the shell) so a dialog
    // dismisses itself without reaching into the window. Owned here; never
    // deleted (the shell outlives it).
    ExtensionDialogBridge *dialog_bridge_ = nullptr;
};
