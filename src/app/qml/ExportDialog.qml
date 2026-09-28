// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The export dialog: a dimmed overlay with the render form (preset, size,
// rate, quality, output path), a progress bar bound to the ExportController,
// a Cancel button while a run is live, and the validation/error line. Pure
// presentation - every value writes through the controller's properties and
// every read comes back from them; the dialog owns no render state.
//
// Hand-rolled like the rest of the shell (no QtQuick.Controls): chips are
// ToggleButtons, inputs are InputField, buttons are Button, and the progress
// bar is two Rectangles. Deliberately NOT in scope for this increment: a
// file-picker for the output path (the path is typed) and an audio-codec
// selector (the timeline is video-only); the preset is the codec choice.

import QtQuick

Rectangle {
    id: dialog

    // The validation problems for the current form, re-derived whenever a
    // field validate() consults changes. Reading the properties registers the
    // dependencies; the one call to the controller is the single source of
    // truth.
    readonly property var problems: {
        var _ = [exporter.preset, exporter.width, exporter.height, exporter.rateNum, exporter.rateDen,
                 exporter.outputPath];
        return exporter.validate();
    }

    function close() {
        visible = false;
    }
    function open() {
        visible = true;
    }

    anchors.fill: parent
    color: Theme.scrim
    visible: false

    // Block input to the panel behind while the dialog is open.
    MouseArea {
        anchors.fill: parent
    }
    Panel {
        id: panel

        anchors.centerIn: parent
        height: 540
        radius: Theme.radiusLarge
        width: 460

        Column {
            anchors.fill: parent
            anchors.margins: 20
            spacing: 10

            Text {
                color: Theme.textPrimary
                font.bold: true
                font.pixelSize: Theme.fsTitle
                text: "Export"
            }
            Rectangle {
                color: Theme.panelBorder
                height: 1
                width: parent.width
            }

            // Preset
            Column {
                spacing: 4
                width: parent.width

                Text {
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fs
                    text: "Preset"
                }
                Flow {
                    spacing: 6
                    width: parent.width

                    Repeater {
                        model: exporter.presets

                        ToggleButton {
                            checked: exporter.preset === modelData
                            label: modelData

                            onClicked: exporter.preset = modelData
                        }
                    }
                }
            }

            // Size
            Column {
                spacing: 4
                width: parent.width

                Text {
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fs
                    text: "Size"
                }
                Row {
                    spacing: 8

                    NumberField {
                        label: "Width"
                        prop: "width"
                        target: exporter
                    }
                    NumberField {
                        label: "Height"
                        prop: "height"
                        target: exporter
                    }
                }
            }

            // Frame rate
            Column {
                spacing: 4
                width: parent.width

                Text {
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fs
                    text: "Frame rate"
                }
                Row {
                    spacing: 8

                    NumberField {
                        label: "Num"
                        prop: "rateNum"
                        target: exporter
                    }
                    NumberField {
                        label: "Den"
                        prop: "rateDen"
                        target: exporter
                    }
                }
            }

            // Quality (informational today: the engine owns encode settings).
            Column {
                spacing: 4
                width: parent.width

                Text {
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fs
                    text: "Quality"
                }
                Text {
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fsMd
                    text: exporter.quality
                }
            }

            // Output path
            Column {
                spacing: 4
                width: parent.width

                Text {
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fs
                    text: "Output"
                }
                InputField {
                    text: exporter.outputPath
                    width: parent.width

                    onEditingFinished: exporter.outputPath = text
                }
            }

            // Validation / error line
            Text {
                color: Theme.danger
                font.pixelSize: Theme.fs
                text: dialog.problems.length > 0 ? dialog.problems.join(" · ") : exporter.error
                visible: text.length > 0
                width: parent.width
                wrapMode: Text.WordWrap
            }

            // Progress bar, shown only while a run is live.
            Column {
                spacing: 4
                visible: exporter.running
                width: parent.width

                Text {
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fs
                    text: exporter.status
                }
                Rectangle {
                    color: Theme.control
                    height: 8
                    radius: 4
                    width: parent.width

                    Rectangle {
                        color: Theme.accent
                        height: parent.height
                        radius: 4
                        width: parent.width * exporter.progress
                    }
                }
            }

            // Buttons: Cancel while running, Export/Close otherwise.
            Row {
                layoutDirection: Qt.RightToLeft
                spacing: 8
                width: parent.width

                Button {
                    accent: true
                    enabled: dialog.problems.length === 0 && !exporter.running
                    label: "Export"

                    onClicked: exporter.startExport()
                }
                Button {
                    label: "Cancel"
                    visible: exporter.running

                    onClicked: exporter.cancelExport()
                }
                Button {
                    label: "Close"
                    visible: !exporter.running

                    onClicked: dialog.close()
                }
            }
        }
    }

    // A labelled, bordered number input that commits on editing-finished (so
    // typing is not an edit per keystroke). `target[prop]` reads and writes
    // the bound controller property, keeping this file free of per-field
    // plumbing.
    component NumberField: Column {
        property int fieldWidth: 90
        property string label: ""
        property string prop: ""
        property var target: null

        spacing: 2

        Text {
            color: Theme.textSecondary
            font.pixelSize: Theme.fsSm
            text: label
        }
        InputField {
            text: target ? String(target[prop]) : ""
            width: fieldWidth

            input.validator: IntValidator {
                bottom: 0
            }

            onEditingFinished: {
                if (target && parseInt(text) >= 0)
                    target[prop] = parseInt(text);
            }
        }
    }
}
