// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The extensions dialog: a dimmed overlay (Panel-based, like SettingsDialog)
// presenting the `extensions` controller's projection - every installed native
// extension with its identity, origin, effective state and the reason it is
// not enabled, plus the switches that change that state, and the tombstoned
// builtins the user can restore.
//
// The controller projects one row per extension:
//   { id, name, version, origin, state, reason, requires, dependents,
//     consented }
// where `origin` is builtin/curated/developer/user and `state` is
// enabled/disabled/revoked/failed. `reason` is why a non-enabled extension is
// not enabled (empty when enabled; a revoked row carries the publisher's
// reason). `requires`/`dependents` are the ids this extension depends on / that
// depend on it, shown as one dependency line.
// `consented` records Developer consent. `removedBuiltins` projects the
// tombstoned builtin ids (one map `{ id }` each) the Removed section restores.
//
// Three controls, all routed through the controller:
//   - a consent toggle, shown only for a Developer-origin row, calls
//     grantConsent/revokeConsent (consent is the gate that keeps a local-only
//     extension out of the process);
//   - an enable/disable toggle on every non-failed row calls
//     setEnabled(id, bool), which the controller keys by origin (Developer ->
//     consent, else the store's disabled list);
//   - a Remove button on every removable row (builtin/curated/developer)
//     confirms inline then calls removeExtension(id).
// A failed or revoked row cannot be toggled: its reason is shown instead, so
// the user sees why rather than a switch that would do nothing. A
// dependency-propagated disable shows its `requires <id>` reason in the same
// reason line.
//
// Install: the header's "Install extension…" button opens a FolderDialog; the
// chosen directory is handed to extensions.install(dir), which validates and
// copies it into the store and re-scans. The result is shown inline (success
// in the success hue, a refusal in the danger hue). A Developer install lands
// consent-gated, so the new row appears failed with a "consent" reason and its
// Consent toggle is the obvious next step; the status line says so.
//
// Two more sections ride the same `extensions.call` bridge:
//   - Marketplace: a catalog URL is fetched through `extensions.catalog`; the
//     entries list with a per-row Install that calls the controller's
//     `installFromCatalog(url, id)` (the catalog-aware install, so the
//     catalog's revocation list is persisted and a revoked entry is refused),
//     which re-scans on success. The URL is persisted through the `settings`
//     facade (`settings.catalogUrl`), so the field survives a restart;
//     fetch/install errors and an empty catalog are shown inline.
//   - Config profile: Export writes the `config.export` document to a chosen
//     file; Import reads a chosen file and hands it to `config.import`, then
//     summarises the report (installed/skipped/failed plus the first failure).
//     The bridge carries the profile inline, so the dialog does the file I/O
//     through the controller's readTextFile/writeTextFile helpers.
//
// Hand-rolled like the rest of the shell (no QtQuick.Controls): chips are
// ToggleButtons, buttons are Button.

import QtQuick
import QtQuick.Dialogs

Rectangle {
    id: dialog

    function close() {
        visible = false;
    }
    function open() {
        visible = true;
    }

    anchors.fill: parent
    color: Theme.scrim
    visible: false

    // Block input to the shell behind the dialog.
    MouseArea {
        anchors.fill: parent
    }

    // The marketplace state and calls. The bridge returns a JSON-RPC envelope
    // as a string: `{result: ...}` on success, `{error: {code, message}}` on
    // failure. `extensions.catalog`'s result is the entries array itself;
    // `extensions.install`'s result is `{installed: "<path>"}`.
    QtObject {
        id: catalog

        property var entries: []
        property bool fetching: false
        property bool isError: false
        property string status: ""

        function fetch() {
            const url = catalogUrl.text.trim();
            if (url === "") {
                entries = [];
                isError = true;
                status = "Enter a catalog URL first.";
                return;
            }
            // Persist the URL before fetching so a successful (or attempted)
            // fetch is remembered, matching the Settings dialog's save-on-edit.
            settings.catalogUrl = url;
            // The fetch is synchronous on this thread; the label flips to
            // "Fetching…" before the call so the wait is visible.
            fetching = true;
            status = "Fetching…";
            isError = false;
            const reply = JSON.parse(extensions.call("extensions.catalog", JSON.stringify({
                                                                                              url: url
                                                                                          })));
            fetching = false;
            if (reply.error) {
                entries = [];
                isError = true;
                status = "Fetch failed: " + reply.error.message;
                return;
            }
            entries = reply.result || [];
            isError = false;
            status = entries.length === 0 ? "The catalog lists no extensions." : "Fetched "
                                            + entries.length + " extension(s).";
        }
        function install(entry) {
            // Route through the controller's catalog-aware install, which uses
            // the shared install_from_catalog path: it fetches the catalog
            // again (so the revocation list is current), refuses a revoked
            // entry, and persists the catalog's revocation list so a later scan
            // forces any revoked extension off. A Developer install lands
            // consent-gated; the controller re-scans on success.
            const result = extensions.installFromCatalog(catalogUrl.text.trim(), entry.id);
            if (result.startsWith("error:")) {
                isError = true;
                status = "Install failed: " + result.substring("error:".length);
                return;
            }
            isError = false;
            status = "Installed " + entry.id + " — grant consent below to load it.";
        }
    }

    // The config-profile state and calls. `config.export` returns the profile
    // document inline; `config.import` takes it inline as `{profile: ...}` and
    // returns `{settings: [...], extensions: [{id, status, reason}]}`.
    QtObject {
        id: profile

        property bool isError: false
        property string status: ""

        function exportTo(path) {
            const reply = JSON.parse(extensions.call("config.export", "{}"));
            if (reply.error) {
                isError = true;
                status = "Export failed: " + reply.error.message;
                return;
            }
            const problem = extensions.writeTextFile(path, JSON.stringify(reply.result, null, 2)
                                                     + "\n");


            if (problem !== "") {
                isError = true;
                status = "Export failed: " + problem;
                return;
            }
            isError = false;
            status = "Exported profile to " + path;
        }
        function importFrom(path) {
            const text = extensions.readTextFile(path);
            if (text === "") {
                isError = true;
                status = "Import failed: cannot read " + path;
                return;
            }
            let document;
            try {
                document = JSON.parse(text);
            } catch (e) {
                isError = true;
                status = "Import failed: " + path + " is not valid JSON";
                return;
            }
            const reply = JSON.parse(extensions.call("config.import", JSON.stringify({
                                                                                         profile: document
                                                                                     })));
            if (reply.error) {
                isError = true;
                status = "Import failed: " + reply.error.message;
                return;
            }
            const report = reply.result || {};
            const entries = report.extensions || [];
            const installed = entries.filter(e => e.status === "installed").length;
            const skipped = entries.filter(e => e.status === "skipped").length;
            const failed = entries.filter(e => e.status === "failed").length;
            const firstFailure = entries.find(e => e.status === "failed");
            isError = failed > 0;
            status = "Imported: " + installed + " installed, " + skipped + " skipped, " + failed + " failed" + (
                        firstFailure ? " — " + firstFailure.id + ": " + firstFailure.reason : "");
            // An import installs extensions through the store, so re-scan to
            // show them.
            extensions.refresh();
        }
    }
    Panel {
        id: panel

        anchors.centerIn: parent
        height: 700
        radius: Theme.radiusLarge
        width: 620

        // The body scrolls: the marketplace and config-profile sections push
        // the content past the panel, so a Flickable keeps every section
        // reachable. The Close button is pinned below, outside the scroll.
        Flickable {
            id: body

            anchors.bottom: footer.top
            anchors.bottomMargin: 0
            anchors.left: parent.left
            anchors.margins: 20
            anchors.right: parent.right
            anchors.top: parent.top
            clip: true
            contentHeight: content.height
            contentWidth: width

            Column {
                id: content

                spacing: 12
                width: body.width

                Row {
                    height: Theme.controlHeight
                    spacing: 8
                    width: parent.width

                    Text {
                        id: titleText

                        anchors.verticalCenter: parent.verticalCenter
                        color: Theme.textPrimary
                        font.bold: true
                        font.pixelSize: Theme.fsTitle
                        text: "Extensions"
                    }

                    // A spacer pushes the Install button to the far right, so it
                    // reads as the header's action. A Row lays its children out
                    // itself, so the button cannot anchor to the Row's right edge.
                    Item {
                        height: 1
                        width: parent.width - parent.spacing - titleText.implicitWidth
                               - installButton.width
                    }

                    // Install from a directory: the picker hands the chosen folder
                    // to the controller, which validates and copies it.
                    Button {
                        id: installButton

                        anchors.verticalCenter: parent.verticalCenter
                        label: "Install extension…"

                        onClicked: installDialog.open()
                    }
                }
                Rectangle {
                    color: Theme.panelBorder
                    height: 1
                    width: parent.width
                }

                // The install result, shown inline under the header: a success in
                // the success hue, a refusal in the danger hue. Hidden until the
                // first install attempt. A Developer install is consent-gated, so
                // the success text names the Consent toggle as the next step.
                Rectangle {
                    id: installStatus

                    property bool isError: false

                    border.color: Theme.wellBorder
                    color: installStatus.isError ? Theme.dangerWash : Theme.well
                    height: installStatusText.implicitHeight + 16
                    radius: Theme.radius
                    visible: installStatusText.text !== ""
                    width: parent.width

                    Text {
                        id: installStatusText

                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        color: installStatus.isError ? Theme.textPrimary : Theme.success
                        font.pixelSize: Theme.fsMd
                        text: ""
                        wrapMode: Text.Wrap
                    }
                }

                // The security note: a native extension is arbitrary code in this
                // process, so the trust boundary is the consent switch below.
                Text {
                    color: Theme.danger
                    font.pixelSize: Theme.fsSm
                    text: "Native extensions run with full trust; a faulty one can crash the app."
                    width: parent.width
                    wrapMode: Text.Wrap
                }

                // ── Marketplace ──────────────────────────────────────────────────
                // A catalog URL is fetched through the `extensions` bridge
                // (`extensions.catalog`), which returns the parsed entries inline.
                // Each entry offers Install, which routes through the same bridge
                // (`extensions.install`) with the entry's source and a Developer
                // origin, then re-scans the store so the new row appears below.
                // The URL is persisted through `settings.catalogUrl`: the field
                // loads the saved value and saves it on fetch (and on edit, like
                // the Settings dialog's other fields).
                Column {
                    spacing: 6
                    width: parent.width

                    Text {
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fs
                        text: "Marketplace"
                    }
                    Row {
                        spacing: 8
                        width: parent.width

                        InputField {
                            id: catalogUrl

                            accessibleName: "Catalog URL"
                            focus: true
                            placeholder: "Catalog URL (https://… or file://…)"
                            text: settings.catalogUrl
                            width: parent.width - fetchButton.width - parent.spacing

                            onEditingFinished: {
                                settings.catalogUrl = catalogUrl.text.trim();
                                catalog.fetch();
                            }
                        }
                        Button {
                            id: fetchButton

                            enabled: !catalog.fetching
                            label: catalog.fetching ? "Fetching…" : "Fetch"

                            onClicked: catalog.fetch()
                        }
                    }
                    Text {
                        color: Theme.textDisabled
                        font.pixelSize: Theme.fsSm
                        text: "A catalog is a JSON document listing installable extensions."
                        width: parent.width
                        wrapMode: Text.Wrap
                    }

                    // The fetch/install status: a success in the success hue, a
                    // refusal in the danger hue. Hidden until the first attempt.
                    Rectangle {
                        border.color: Theme.wellBorder
                        color: catalog.isError ? Theme.dangerWash : Theme.well
                        height: catalogStatusText.implicitHeight + 16
                        radius: Theme.radius
                        visible: catalogStatusText.text !== ""
                        width: parent.width

                        Text {
                            id: catalogStatusText

                            anchors.left: parent.left
                            anchors.leftMargin: 10
                            anchors.right: parent.right
                            anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            color: catalog.isError ? Theme.textPrimary : Theme.success
                            font.pixelSize: Theme.fsMd
                            text: catalog.status
                            wrapMode: Text.Wrap
                        }
                    }

                    // The fetched entries, one row each: name + version, id, and
                    // the description, with an Install button. Empty until a fetch
                    // succeeds; the status line above says why when it does not.
                    Repeater {
                        model: catalog.entries

                        delegate: Rectangle {
                            id: catalogRow

                            border.color: Theme.wellBorder
                            color: Theme.well
                            height: 56
                            radius: Theme.radius
                            width: parent.width

                            Column {
                                anchors.left: parent.left
                                anchors.leftMargin: 10
                                anchors.right: installEntryButton.left
                                anchors.rightMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 2

                                Text {
                                    color: Theme.textPrimary
                                    elide: Text.ElideRight
                                    font.pixelSize: Theme.fsMd
                                    text: modelData.name + "  v" + modelData.version
                                    width: parent.width
                                }
                                Text {
                                    color: Theme.textSecondary
                                    elide: Text.ElideRight
                                    font.family: Theme.fontTechnical
                                    font.pixelSize: Theme.fsSm
                                    text: modelData.id
                                    width: parent.width
                                }
                                Text {
                                    color: Theme.textSecondary
                                    elide: Text.ElideRight
                                    font.pixelSize: Theme.fsSm
                                    text: modelData.description
                                    visible: modelData.description !== ""
                                    width: parent.width
                                }
                            }
                            Button {
                                id: installEntryButton

                                anchors.right: parent.right
                                anchors.rightMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                label: "Install"

                                onClicked: catalog.install(modelData)
                            }
                        }
                    }
                }

                // The installed extensions, one row each. A row shows identity,
                // origin, state, its requires/dependents, the reason it is not
                // enabled, and the switches.
                Column {
                    spacing: 6
                    width: parent.width

                    Text {
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fs
                        text: "Installed (" + extensions.extensions.length + ")"
                    }
                    Text {
                        color: Theme.textDisabled
                        font.pixelSize: Theme.fsMd
                        text: "No extensions are installed."
                        visible: extensions.extensions.length === 0
                        width: parent.width
                    }
                    Repeater {
                        model: extensions.extensions

                        delegate: Rectangle {
                            id: extRow

                            property bool hasDependencies: modelData.requires.length > 0
                                                           || modelData.dependents.length > 0
                            // Each extra line (the dependency line, the reason
                            // line) adds a line height to the base two-line row.
                            property bool removeArmed: false

                            border.color: (modelData.state === "failed" || modelData.state
                                           === "revoked") ? Theme.danger : Theme.wellBorder
                            color: Theme.well
                            height: 56 + (extRow.hasDependencies ? 18 : 0) + (modelData.reason !== ""
                                                                              ? 18 : 0)
                            radius: Theme.radius
                            width: parent.width

                            // The identity block: name + version, then id, origin
                            // and state, then the dependency line, then the reason
                            // a non-enabled extension is not enabled. The state
                            // word is tinted by state so a failed row reads at a
                            // glance.
                            Column {
                                anchors.left: parent.left
                                anchors.leftMargin: 10
                                anchors.right: switches.left
                                anchors.rightMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 2

                                Text {
                                    color: Theme.textPrimary
                                    elide: Text.ElideRight
                                    font.pixelSize: Theme.fsMd
                                    text: modelData.name + "  v" + modelData.version
                                    width: parent.width
                                }
                                Text {
                                    color: (modelData.state === "failed" || modelData.state
                                            === "revoked") ? Theme.danger : (modelData.state
                                                                             === "enabled"
                                                                             ? Theme.success :
                                                                               Theme.textSecondary)
                                    elide: Text.ElideRight
                                    font.family: Theme.fontTechnical
                                    font.pixelSize: Theme.fsSm
                                    text: modelData.id + "  ·  " + modelData.origin + "  ·  "
                                          + modelData.state
                                    width: parent.width
                                }

                                // The dependency line: who this requires and who
                                // depends on it, so an enable/remove cascade is
                                // traceable before it happens.
                                Text {
                                    color: Theme.textSecondary
                                    elide: Text.ElideRight
                                    font.family: Theme.fontTechnical
                                    font.pixelSize: Theme.fsSm
                                    text: (modelData.requires.length > 0 ? "requires: "
                                                                           + modelData.requires.join(
                                                                               ", ") : "") + (
                                              modelData.requires.length > 0
                                              && modelData.dependents.length > 0 ? "  ·  " : "") + (
                                              modelData.dependents.length > 0 ? "used by: "
                                                                                + modelData.dependents.join(
                                                                                    ", ") : "")
                                    visible: extRow.hasDependencies
                                    width: parent.width
                                }

                                // The reason a non-enabled extension is not
                                // enabled (a refusal, a missing consent, a bad
                                // manifest/library, or a dependency that went
                                // down as `requires <id>`). Empty when enabled.
                                Text {
                                    color: Theme.textSecondary
                                    elide: Text.ElideRight
                                    font.pixelSize: Theme.fsSm
                                    maximumLineCount: 2
                                    text: modelData.reason
                                    visible: modelData.reason !== ""
                                    width: parent.width
                                    wrapMode: Text.Wrap
                                }
                            }

                            // The switches, right-aligned. A failed or revoked row
                            // has no enable/consent switch (its reason is the
                            // whole story); a Developer row gets the consent
                            // toggle; every non-failed, non-revoked row gets the
                            // enable/disable toggle; every removable row (not
                            // user-origin) gets the Remove button.
                            Row {
                                id: switches

                                anchors.right: parent.right
                                anchors.rightMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 6

                                // Consent is the Developer gate: granting loads
                                // the extension, revoking unloads it. Shown only
                                // for a Developer-origin row that is neither
                                // failed nor revoked (a revocation is a
                                // forced-off, not a consent question).
                                ToggleButton {
                                    checked: modelData.consented
                                    label: modelData.consented ? "Consented" : "Consent"
                                    visible: modelData.origin === "developer" && modelData.state
                                             !== "failed" && modelData.state !== "revoked"

                                    onClicked: {
                                        extRow.removeArmed = false;
                                        if (modelData.consented)
                                            extensions.revokeConsent(modelData.id);
                                        else
                                            extensions.grantConsent(modelData.id);
                                    }
                                }

                                // The single enable/disable switch, keyed by
                                // origin inside the controller. A failed or
                                // revoked row is not toggleable. The label
                                // names the action the click performs.
                                ToggleButton {
                                    checked: modelData.state === "enabled"
                                    label: modelData.state === "enabled" ? "Disable" : "Enable"
                                    visible: modelData.state !== "failed" && modelData.state
                                             !== "revoked"

                                    onClicked: {
                                        extRow.removeArmed = false;
                                        extensions.setEnabled(modelData.id, modelData.state
                                                              !== "enabled");
                                    }
                                }

                                // Remove, with a one-click inline confirmation
                                // (arm, then confirm). Builtins and Curated/
                                // Developer rows are removable; a User-origin row
                                // is refused at install and never removable.
                                Button {
                                    accent: extRow.removeArmed
                                    label: extRow.removeArmed ? "Confirm" : "Remove"
                                    visible: modelData.origin !== "user"

                                    onClicked: {
                                        if (!extRow.removeArmed) {
                                            extRow.removeArmed = true;
                                        } else {
                                            extRow.removeArmed = false;
                                            extensions.removeExtension(modelData.id);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // The removed builtins: ids tombstoned on the store's removed.json
                // whose origin is builtin. Each offers Restore, which clears the
                // tombstone and re-seeds the builtin. Hidden when nothing is
                // removed.
                Column {
                    spacing: 6
                    visible: extensions.removedBuiltins.length > 0
                    width: parent.width

                    Text {
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fs
                        text: "Removed (" + extensions.removedBuiltins.length + ")"
                    }
                    Repeater {
                        model: extensions.removedBuiltins

                        delegate: Rectangle {
                            border.color: Theme.wellBorder
                            color: Theme.well
                            height: 40
                            radius: Theme.radius
                            width: parent.width

                            Text {
                                anchors.left: parent.left
                                anchors.leftMargin: 10
                                anchors.right: restoreBtn.left
                                anchors.rightMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                color: Theme.textSecondary
                                elide: Text.ElideRight
                                font.family: Theme.fontTechnical
                                font.pixelSize: Theme.fsSm
                                text: modelData.id + "  ·  removed"
                            }
                            Button {
                                id: restoreBtn

                                anchors.right: parent.right
                                anchors.rightMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                label: "Restore"

                                onClicked: extensions.restoreExtension(modelData.id)
                            }
                        }
                    }
                }

                // The refusal count: installed extensions the host would not load
                // (untrusted, no consent, or a bad manifest/library). The per-row
                // reason above names each one; this is the aggregate.
                Rectangle {
                    border.color: Theme.wellBorder
                    color: extensions.refusalCount > 0 ? Theme.dangerWash : Theme.well
                    height: refusalText.implicitHeight + 16
                    radius: Theme.radius
                    width: parent.width

                    Text {
                        id: refusalText

                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        color: extensions.refusalCount > 0 ? Theme.textPrimary : Theme.textSecondary
                        font.pixelSize: Theme.fsMd
                        text: extensions.refusalCount > 0 ? extensions.refusalCount
                                                            + " installed extension(s) refused to load — see the reason on each row." :
                                                            "No extensions were refused."
                        wrapMode: Text.Wrap
                    }
                }

                // ── Config profile ───────────────────────────────────────────────
                // Export writes the machine's portable profile (settings plus
                // installed extensions) to a chosen file; import reads one back and
                // applies it. The bridge verbs carry the profile inline, so the
                // dialog does the file I/O itself: export fetches the document and
                // writes it, import reads the file and hands the parsed object to
                // `config.import`. The import report is summarised as counts plus
                // the first failure reason.
                Column {
                    spacing: 6
                    width: parent.width

                    Text {
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fs
                        text: "Config profile"
                    }
                    Row {
                        spacing: 8
                        width: parent.width

                        Button {
                            label: "Export profile…"

                            onClicked: exportProfileDialog.open()
                        }
                        Button {
                            label: "Import profile…"

                            onClicked: importProfileDialog.open()
                        }
                    }
                    Text {
                        color: Theme.textDisabled
                        font.pixelSize: Theme.fsSm
                        text: "A profile reproduces this machine's settings and installed extensions elsewhere."
                        width: parent.width
                        wrapMode: Text.Wrap
                    }

                    // The export/import status: a success in the success hue, a
                    // refusal in the danger hue. Hidden until the first attempt.
                    Rectangle {
                        border.color: Theme.wellBorder
                        color: profile.isError ? Theme.dangerWash : Theme.well
                        height: profileStatusText.implicitHeight + 16
                        radius: Theme.radius
                        visible: profileStatusText.text !== ""
                        width: parent.width

                        Text {
                            id: profileStatusText

                            anchors.left: parent.left
                            anchors.leftMargin: 10
                            anchors.right: parent.right
                            anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            color: profile.isError ? Theme.textPrimary : Theme.success
                            font.pixelSize: Theme.fsMd
                            text: profile.status
                            wrapMode: Text.Wrap
                        }
                    }
                }

                // The author guide, as a path the user can open themselves; the
                // shell has no in-app doc viewer.
                Text {
                    color: Theme.textSecondary
                    font.family: Theme.fontTechnical
                    font.pixelSize: Theme.fsSm
                    text: "Author guide: docs/extensions.md"
                    width: parent.width
                }
            }
        }

        // The pinned footer: Close stays reachable however far the body
        // scrolls.
        Row {
            id: footer

            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.margins: 20
            anchors.right: parent.right
            height: Theme.controlHeight
            layoutDirection: Qt.RightToLeft
            spacing: 8

            Button {
                label: "Close"

                onClicked: dialog.close()
            }
        }
    }

    // The install picker: a directory holding an extension.toml. The controller
    // validates the manifest and detects whether an `entry` is present; a
    // refusal writes nothing to the store. The result is shown in the status
    // line above, and a Developer install's new row appears consent-gated.
    FolderDialog {
        id: installDialog

        title: "Install extension"

        onAccepted: {
            const result = extensions.install(selectedFolder);
            installStatus.isError = result.startsWith("error:");
            installStatusText.text = result;
        }
    }

    // The profile export picker: the chosen path receives the exported
    // document. The dialog writes it through the controller's file helper,
    // because the bridge carries the profile inline rather than by path.
    FileDialog {
        id: exportProfileDialog

        defaultSuffix: "json"
        fileMode: FileDialog.SaveFile
        title: "Export config profile"

        onAccepted: profile.exportTo(selectedFile)
    }

    // The profile import picker: the chosen file is read and handed to
    // `config.import` inline.
    FileDialog {
        id: importProfileDialog

        fileMode: FileDialog.OpenFile
        title: "Import config profile"

        onAccepted: profile.importFrom(selectedFile)
    }
}
