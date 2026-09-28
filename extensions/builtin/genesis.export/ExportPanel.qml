// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The export form as an inspector panel (the builtin `genesis.export`
// extension). It is the QML port of the old ExportDialog, compacted to fit the
// short inspector pane (~185px): the preset is a single dropdown row, the
// output path and its Browse button share one row, the size/rate overrides sit
// behind a collapsed "Options" disclosure, and the Export/Cancel action row is
// pinned to the bottom. The progress bar and the outcome line replace the
// preset row while a run is live, so the pane never grows past its budget.
//
// The panel owns no host state. It reaches the host through the `extensions`
// context property's `call(method, paramsJson)` bridge, which returns the same
// JSON-RPC reply envelope the CLI sees:
//
//   * `export.presets` -> {"result": {presets: [{name, width, height, rateNum,
//                          rateDen, fps, codec, quality}]}}
//   * `export.run`     -> {"result": {job, path, output}} or {"error": {...}}
//   * `export.status`  -> {"result": {job, running, progress, status, error}}
//   * `export.cancel`  -> {"result": {}}
//
// The old dialog bound to the ExportController's properties directly; the
// bridge has no push channel, so the panel polls `export.status` on a ~4 Hz
// Timer while a run is live (the old dialog repainted on the controller's
// progressChanged signal). `export.status` carries `running`, `progress`
// (0..1), `status` ("Idle"/"Exporting"/"Done"/"Cancelled"/"Failed") and
// `error`; it has no stage/frame field, so the panel shows the status word and
// the fraction, not a per-frame stage.
//
// The panel is loaded from the installed-extension store, outside the app's
// QML directory, so it imports the app module (`GenesisApp`) for the design
// system (Theme, Button, InputField) and reads the shared context property
// (`extensions`). A preset list that is unavailable or empty degrades to a
// visible message, never a blank pane.

import QtQuick
import QtQuick.Dialogs
import GenesisApp 1.0

Item {
    id: panel

    // The last error the host returned, shown in the danger colour. Empty when
    // the last call succeeded.
    property string errorText: ""

    // The size/rate overrides. Zero means "leave it to the preset" (the host
    // treats an absent spec field as the timeline's default), so the fields
    // start blank and only a positive entry is sent. Named `out*` because
    // `width`/`height` are final members of the Item base type.
    property int outHeight: 0
    property int outRateDen: 0
    property int outRateNum: 0
    property int outWidth: 0

    // The output path, a plain local path (the FileDialog's file:// URL is
    // converted on accept). Empty until the user picks one.
    property string outputPath: ""

    // The selected preset name, or "" before the first `export.presets` reply.
    property string preset: ""

    // The discovered presets, one map per preset (see the header). Empty until
    // the first reply lands, and empty again if that call errors.
    property var presets: []

    // The validation problems for the current form, mirroring the old dialog's
    // `exporter.validate()`: a preset must be picked and the output path must
    // be non-empty. The host re-validates on `export.run`; this only gates the
    // button so the reason is visible before the call.
    readonly property var problems: {
        const out = [];
        if (preset === "")
            out.push("Pick a preset");
        if (outputPath === "")
            out.push("Choose an output file");
        return out;
    }

    // The run's read-only view, folded from `export.status`. `progress` is
    // 0..1; `status` is the host's word; `running` gates the poll and the
    // buttons.
    property double progress: 0.0

    // The quality hint of the selected preset, or "".
    readonly property string quality: selectedPreset ? selectedPreset.quality : ""
    property bool running: false

    // The selected preset map, or null.
    readonly property var selectedPreset: {
        for (let i = 0; i < presets.length; ++i) {
            if (presets[i].name === preset)
                return presets[i];
        }
        return null;
    }
    property string status: "Idle"

    // Cancel the running export. The host keeps `running` true until the
    // worker confirms it stopped, so the poll continues until then.
    function cancelExport() {
        if (reply.resultOf(extensions.call("export.cancel", "{}")) === null)
            return;
        poll();
    }

    // Discover the presets. Called once on completion; the preset set is
    // static, so there is no per-edit refresh.
    function loadPresets() {
        panel.errorText = "";
        const result = reply.resultOf(extensions.call("export.presets", "{}"));
        if (result === null) {
            panel.presets = [];
            panel.preset = "";
            return;
        }
        panel.presets = result.presets !== undefined ? result.presets : [];
        panel.preset = panel.presets.length > 0 ? panel.presets[0].name : "";
    }

    // Convert a FileDialog's file:// URL to a plain local path. The dialog
    // hands back a percent-encoded URL; the host's `spec.output` is a plain
    // path, so the encoding is undone here.
    function localPath(url) {
        let path = url.toString();
        if (path.startsWith("file://"))
            path = path.slice(7);
        try {
            return decodeURIComponent(path);
        } catch (e) {
            return path;
        }
    }

    // Fold one `export.status` reply into the panel's view. A malformed or
    // refused reply sets `errorText` and leaves the last good state in place,
    // so a transient failure does not blank the progress bar.
    function poll() {
        const result = reply.resultOf(extensions.call("export.status", "{}"));
        if (result === null)
            return;
        panel.running = result.running === true;
        panel.progress = result.progress !== undefined ? result.progress : 0.0;
        panel.status = result.status !== undefined ? result.status : "Idle";
        if (result.error !== undefined && result.error !== "")
            panel.errorText = result.error;
    }

    // Start a run. The spec carries only the fields the user set: `preset` and
    // `output` always, the size/rate overrides only when positive (the host
    // reads an absent field as the timeline's default).
    function startExport() {
        if (problems.length > 0 || running)
            return;
        panel.errorText = "";
        const spec = {
            preset: preset,
            output: outputPath
        };
        if (outWidth > 0)
            spec.width = outWidth;
        if (outHeight > 0)
            spec.height = outHeight;
        if (outRateNum > 0)
            spec.rateNum = outRateNum;
        if (outRateDen > 0)
            spec.rateDen = outRateDen;
        const result = reply.resultOf(extensions.call("export.run", JSON.stringify({
                                                                                       spec: spec
                                                                                   })));
        if (result === null)
            return;
        // The run is live from the host's point of view; the first poll folds
        // the real state in.
        panel.running = true;
        panel.status = "Exporting";
        panel.progress = 0.0;
        poll();
    }

    Component.onCompleted: loadPresets()

    AiReply {
        id: reply

        errorTarget: panel
    }

    // ~4 Hz while a run is live: the old dialog repainted on the controller's
    // progressChanged signal; the bridge is a synchronous call, so a slower
    // poll keeps the UI thread free while still reading as live. The timer
    // stops once the host reports the run is no longer running.
    Timer {
        interval: 250
        repeat: true
        running: panel.running && panel.visible

        onTriggered: panel.poll()
    }

    // ── the compact form ─────────────────────────────────────────────────
    // The inspector pane's panel slot is only ~100px tall in the 1280x760
    // shell (the 220px clip list and the tab bar take the rest), so the form is
    // two rows and nothing else: preset + Options + Export on the first, output
    // + Browse on the second. There is no title (the tab already says "Export")
    // and no field labels (the dropdown's own text and the field's placeholder
    // name the controls). The overrides live in an overlay (below) so opening
    // them never reflows the two rows.
    Column {
        id: form

        anchors.left: parent.left
        anchors.leftMargin: Theme.pad
        anchors.right: parent.right
        anchors.rightMargin: Theme.pad
        anchors.top: parent.top
        anchors.topMargin: Theme.pad
        spacing: Theme.spacing

        // The host's refusal, shown only when a call failed.
        Text {
            color: Theme.danger
            font.pixelSize: Theme.fs
            text: panel.errorText
            visible: panel.errorText !== ""
            width: parent.width
            wrapMode: Text.Wrap
        }

        // Empty state: the preset source is unavailable or has no presets.
        Text {
            color: Theme.textDisabled
            font.pixelSize: Theme.fsMd
            text: "No export presets available."
            visible: panel.presets.length === 0
            width: parent.width
            wrapMode: Text.Wrap
        }

        // ── row 1: preset dropdown + Options + Export ────────────────────
        // The preset is a single dropdown button (the list opens as an overlay,
        // so eight presets never wrap into a chip grid); Options is a compact
        // disclosure; Export is the primary action, right-aligned.
        Row {
            id: primaryRow

            spacing: 6
            visible: panel.presets.length > 0
            width: parent.width

            Rectangle {
                id: presetButton

                Accessible.description: "Opens the preset list"
                Accessible.name: panel.preset !== "" ? "Preset: " + panel.preset : "Choose a preset"
                Accessible.role: Accessible.ComboBox
                border.color: presetMenu.visible ? Theme.accentBorder : Theme.controlBorder
                color: presetHover.containsMouse ? Theme.controlHover : Theme.control
                height: Theme.controlHeight
                width: parent.width - optionsToggle.width - exportButton.width - 2 * parent.spacing

                Accessible.onPressAction: presetMenu.visible = !presetMenu.visible

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.right: chevron.left
                    anchors.rightMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    color: Theme.textPrimary
                    elide: Text.ElideRight
                    font.pixelSize: Theme.fsLg
                    text: panel.preset !== "" ? panel.preset : "Choose a preset…"
                }
                Text {
                    id: chevron

                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fsMd
                    text: presetMenu.visible ? "\u25b2" : "\u25bc"
                }
                MouseArea {
                    id: presetHover

                    anchors.fill: parent
                    hoverEnabled: true

                    onClicked: presetMenu.visible = !presetMenu.visible
                }
            }
            Rectangle {
                id: optionsToggle

                Accessible.checkable: true
                Accessible.checked: optionsOverlay.visible
                Accessible.name: "Options"
                Accessible.role: Accessible.CheckBox
                border.color: optionsOverlay.visible ? Theme.accentBorder : Theme.controlBorder
                color: optionsHover.containsMouse ? Theme.controlHover : Theme.control
                height: Theme.controlHeight
                radius: Theme.radius
                width: optionsLabel.implicitWidth + 16

                Accessible.onPressAction: optionsOverlay.visible = !optionsOverlay.visible

                Text {
                    id: optionsLabel

                    anchors.centerIn: parent
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fsMd
                    text: optionsOverlay.visible ? "\u25b2 Options" : "\u25bc Options"
                }
                MouseArea {
                    id: optionsHover

                    anchors.fill: parent
                    hoverEnabled: true

                    onClicked: optionsOverlay.visible = !optionsOverlay.visible
                }
            }
            Button {
                id: exportButton

                accent: true
                enabled: panel.problems.length === 0 && !panel.running
                label: "Export"

                onClicked: panel.startExport()
            }
        }

        // ── row 2: output field + Browse ─────────────────────────────────
        Row {
            spacing: 6
            width: parent.width

            InputField {
                accessibleName: "Output file"
                placeholder: "Output file…"
                text: panel.outputPath
                width: parent.width - chooseButton.width - 6

                onEditingFinished: panel.outputPath = text
            }
            Button {
                id: chooseButton

                label: "Browse…"

                onClicked: outputDialog.open()
            }
        }

        // ── progress, shown only while a run is live ─────────────────────
        Column {
            spacing: 4
            visible: panel.running
            width: parent.width

            Text {
                color: Theme.textSecondary
                font.pixelSize: Theme.fs
                text: panel.status + "  ·  " + Math.round(panel.progress * 100) + "%"
            }
            Rectangle {
                Accessible.name: "Export progress"
                Accessible.role: Accessible.ProgressBar
                Accessible.value: panel.progress
                color: Theme.control
                height: 8
                radius: 4
                width: parent.width

                Rectangle {
                    color: Theme.accent
                    height: parent.height
                    radius: 4
                    width: parent.width * panel.progress
                }
            }
        }

        // ── the outcome, once a run has finished ─────────────────────────
        Text {
            color: panel.status === "Failed" ? Theme.danger : Theme.success
            font.pixelSize: Theme.fs
            text: panel.status === "Done" ? "Export complete." : (panel.status === "Cancelled" ? "Export cancelled." :
                                                                                                 "")
            visible: !panel.running && (panel.status === "Done" || panel.status === "Cancelled")
            width: parent.width
            wrapMode: Text.Wrap
        }
    }

    // ── the Cancel action, pinned to the pane bottom ─────────────────────
    // Only shown while a run is live; it sits below the two form rows so the
    // primary Export button never moves. The Options overlay floats above it.
    Button {
        id: cancelButton

        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.pad
        anchors.left: parent.left
        anchors.leftMargin: Theme.pad
        label: "Cancel"
        visible: panel.running

        onClicked: panel.cancelExport()
    }

    // ── the preset list, an overlay under the preset button ──────────────
    // Anchored to the preset button's bottom edge and drawn above the form
    // (z: 100), so it can extend past the pane without reflowing anything. The
    // button lives inside `form`/`primaryRow`, so its position is mapped into
    // the panel's coordinates.
    Rectangle {
        id: presetMenu

        border.color: Theme.panelBorder
        border.width: 1
        color: Theme.panel
        height: Math.min(presetList.implicitHeight + 8, 220)
        radius: Theme.radius
        visible: false
        width: presetButton.width
        x: presetButton.mapToItem(panel, 0, 0).x
        y: presetButton.mapToItem(panel, 0, 0).y + presetButton.height + 2
        z: 100

        Flickable {
            anchors.fill: parent
            anchors.margins: 4
            clip: true
            contentHeight: presetList.implicitHeight
            contentWidth: width

            Column {
                id: presetList

                spacing: 2
                width: parent.width

                Repeater {
                    model: panel.presets

                    Rectangle {
                        Accessible.checkable: true
                        Accessible.checked: panel.preset === modelData.name
                        Accessible.name: modelData.name
                        Accessible.role: Accessible.MenuItem
                        color: presetRowHover.containsMouse ? Theme.controlHover : (panel.preset
                                                                                    === modelData.name
                                                                                    ? Theme.accentSoft :
                                                                                      "transparent")
                        height: 24
                        radius: Theme.radiusSmall
                        width: presetList.width

                        Accessible.onPressAction: {
                            panel.preset = modelData.name;
                            presetMenu.visible = false;
                        }

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 8
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            color: Theme.textPrimary
                            elide: Text.ElideRight
                            font.pixelSize: Theme.fsMd
                            text: modelData.name
                        }
                        MouseArea {
                            id: presetRowHover

                            anchors.fill: parent
                            hoverEnabled: true

                            onClicked: {
                                panel.preset = modelData.name;
                                presetMenu.visible = false;
                            }
                        }
                    }
                }
            }
        }
    }

    // ── the overrides, an overlay over the form ──────────────────────────
    // A bordered sheet that floats over the two form rows when the Options
    // toggle is open. It carries the size and frame-rate fields; the host reads
    // a blank field as "leave it to the preset". It is anchored to the pane
    // bottom and grows upward, so it never pushes the form rows.
    Rectangle {
        id: optionsOverlay

        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.pad
        anchors.left: parent.left
        anchors.leftMargin: Theme.pad
        anchors.right: parent.right
        anchors.rightMargin: Theme.pad
        border.color: Theme.panelBorder
        border.width: 1
        color: Theme.panel
        height: optionsColumn.implicitHeight + 2 * Theme.spacing
        radius: Theme.radius
        visible: false
        z: 90

        Column {
            id: optionsColumn

            anchors.left: parent.left
            anchors.leftMargin: Theme.spacing
            anchors.right: parent.right
            anchors.rightMargin: Theme.spacing
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spacing

            Text {
                color: Theme.textSecondary
                font.pixelSize: Theme.fs
                text: "Size (blank = preset)"
            }
            Row {
                spacing: 8

                NumberField {
                    label: "Width"
                    prop: "outWidth"
                }
                NumberField {
                    label: "Height"
                    prop: "outHeight"
                }
            }
            Text {
                color: Theme.textSecondary
                font.pixelSize: Theme.fs
                text: "Frame rate (blank = preset)"
            }
            Row {
                spacing: 8

                NumberField {
                    label: "Num"
                    prop: "outRateNum"
                }
                NumberField {
                    label: "Den"
                    prop: "outRateDen"
                }
            }
        }
    }

    // The output picker. The chosen file:// URL is converted to a plain path
    // for the host's `spec.output`.
    FileDialog {
        id: outputDialog

        fileMode: FileDialog.SaveFile
        title: "Export to file"

        onAccepted: panel.outputPath = panel.localPath(selectedFile)
    }

    // A labelled, bordered number input that commits on editing-finished (so
    // typing is not an edit per keystroke). `panel[prop]` reads and writes the
    // bound property, keeping this file free of per-field plumbing. A blank
    // field writes 0, which the spec builder reads as "leave it to the preset".
    component NumberField: Column {
        property int fieldWidth: 90
        property string label: ""
        property string prop: ""

        spacing: 2

        Text {
            color: Theme.textSecondary
            font.pixelSize: Theme.fsSm
            text: label
        }
        InputField {
            accessibleName: label
            text: panel[prop] > 0 ? String(panel[prop]) : ""
            width: fieldWidth

            input.validator: IntValidator {
                bottom: 0
            }

            onEditingFinished: {
                const v = parseInt(text);
                panel[prop] = isNaN(v) || v < 0 ? 0 : v;
            }
        }
    }
}
