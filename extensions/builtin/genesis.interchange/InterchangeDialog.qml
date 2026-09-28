// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The interchange dialog as a contributed extension dialog (the builtin
// `genesis.interchange` extension). It is deliberately thin: the conversion is
// a host capability (`interchange.export` / `interchange.import`), so the format
// logic lives over the project model in C++ where it is testable without a QML
// engine and usable headlessly. This dialog picks a path and reports the
// outcome - the same shape as the other builtin tools.
//
// The bridge is the `extensions` context property's `call(method, paramsJson)`,
// which returns the same JSON-RPC reply envelope the CLI sees:
//
//   * `interchange.export` -> {"result": {}} or {"error": {code, message}}
//   * `interchange.import` -> {"result": <editor view>}
//
// Export names its format by the file's suffix; import needs no format at all,
// because the host reads it from the content. That is why one button each is
// enough and why the dialog carries no per-format state.
//
// The dialog is loaded from the installed-extension store, outside the app's
// QML directory, so it imports the app module (`GenesisApp`) for the design
// system (Theme, Panel, Button) and reads the shared context property
// (`extensions`). It dismisses itself through `dialogBridge.close()`.

import QtQuick
import QtQuick.Dialogs
import GenesisApp 1.0

Item {
    id: dialog

    // True while a host call is in flight, so the buttons gate.
    property bool busy: false
    // A host refusal lands here through AiReply; mirrored into `message`.
    property string errorText: ""

    // The last outcome, shown above the buttons. Empty until an action runs.
    property string message: ""

    function exportAs(path) {
        dialog.busy = true;
        const done = reply.resultOf(extensions.call("interchange.export", JSON.stringify({
                                                                                             "path": path,
                                                                                             "format": dialog.formatFor(
                                                                                                           path)
                                                                                         })));
        dialog.busy = false;
        if (done !== null)
            dialog.message = "Exported " + path;
    }

    // The format is the suffix the user chose. OTIO is the default because it
    // is the only one that round-trips losslessly.
    function formatFor(path) {
        const lower = path.toLowerCase();
        if (lower.endsWith(".fcpxml") || lower.endsWith(".xml"))
            return "fcpxml";
        if (lower.endsWith(".edl"))
            return "edl";
        return "otio";
    }
    function importFrom(path) {
        dialog.busy = true;
        const loaded = reply.resultOf(extensions.call("interchange.import", JSON.stringify({
                                                                                               "path": path
                                                                                           })));
        dialog.busy = false;
        if (loaded !== null)
            dialog.message = "Imported " + path;
    }

    // A FileDialog hands back a URL; the host verbs take filesystem paths.
    function localPath(url) {
        let text = String(url);
        if (text.indexOf("file://") === 0)
            text = text.substring(7);
        try {
            text = decodeURIComponent(text);
        } catch (e) {
            // Leave a malformed escape as it came in; the host reports the open.
        }
        if (/^\/[A-Za-z]:/.test(text))
            text = text.substring(1);
        return text;
    }

    onErrorTextChanged: {
        if (errorText.length > 0)
            message = errorText;
    }

    // Decodes the host's reply envelope; a refusal writes errorText.
    AiReply {
        id: reply

        errorTarget: dialog
    }
    FileDialog {
        id: exportDialog

        defaultSuffix: "otio"
        fileMode: FileDialog.SaveFile
        nameFilters: ["OpenTimelineIO (*.otio)", "CMX 3600 EDL (*.edl)", "FCPXML (*.fcpxml)",
            "All files (*)"]
        title: "Export timeline"

        onAccepted: dialog.exportAs(dialog.localPath(selectedFile))
    }
    FileDialog {
        id: importDialog

        fileMode: FileDialog.OpenFile
        nameFilters: ["Timeline interchange (*.otio *.edl *.fcpxml *.xml)", "All files (*)"]
        title: "Import timeline"

        onAccepted: dialog.importFrom(dialog.localPath(selectedFile))
    }
    Panel {
        id: panel

        anchors.centerIn: parent
        height: 200
        radius: Theme.radiusLarge
        width: 520

        Column {
            anchors.fill: parent
            anchors.margins: 20
            spacing: 10

            Text {
                color: Theme.textPrimary
                font.bold: true
                font.pixelSize: Theme.fsTitle
                text: "Interchange"
            }
            Rectangle {
                color: Theme.panelBorder
                height: 1
                width: parent.width
            }
            Text {
                color: Theme.textSecondary
                font.pixelSize: Theme.fs
                text: "Exchange the open timeline with other editors. "
                      + "OpenTimelineIO keeps the whole project; EDL and FCPXML carry the cut."
                width: parent.width
                wrapMode: Text.WordWrap
            }
            Text {
                color: dialog.errorText.length > 0 ? Theme.danger : Theme.textSecondary
                elide: Text.ElideMiddle
                font.pixelSize: Theme.fs
                height: 20
                text: dialog.message
                width: parent.width
            }
            Item {
                height: 1
                width: parent.width
            }
            Row {
                layoutDirection: Qt.RightToLeft
                spacing: 8
                width: parent.width

                Button {
                    accent: true
                    enabled: !dialog.busy
                    focus: true
                    label: "Export"

                    onClicked: exportDialog.open()
                }
                Button {
                    enabled: !dialog.busy
                    label: "Import"

                    onClicked: importDialog.open()
                }
                Button {
                    label: "Close"

                    onClicked: dialogBridge.close()
                }
            }
        }
    }
}
