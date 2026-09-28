// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The enhance section of the clip inspector: the selected clip's upscale and
// denoise, driven by the `enhance` controller. It mirrors the shell's
// selection onto the controller's clipId (exactly as CutoutSection drives
// `cutout`) and presents the controller's state machine in place:
//
//   unavailable -> the runtime's own reason (statusText) in the disabled style;
//   Idle        -> the factor chips (1x / 2x / 4x) and Enhance;
//   Consent     -> the model name/size/license/URL with Download / Cancel;
//   Running     -> a progress bar, the current stage and Cancel;
//   Failed      -> the controller's reason in Theme.danger;
//   Done        -> a short success line (the copy appears via the render path).
//
// The consent and progress states are also mirrored by EnhanceDialog in
// Main.qml, which is the modal sheet the File menu opens; this inline section
// is the always-visible inspector affordance. Both read the same controller
// properties, so they can never disagree.
//
// Hand-rolled like the rest of the shell (no QtQuick.Controls): the progress
// bar is two Rectangles, buttons are Button, chips are ToggleButton.

import QtQuick

Column {
    id: section

    readonly property bool cancelled: enhance.state === "Cancelled"

    // The clip the shell has selected; the section mirrors it onto the
    // controller. Empty means nothing is selected.
    property string clipId: ""
    readonly property bool consenting: enhance.state === "Consent"
    readonly property bool done: enhance.state === "Done"
    readonly property bool failed: enhance.state === "Failed"
    readonly property bool running: enhance.state === "Running"

    // The controller's state, read once so the branches below stay declarative.
    readonly property bool unavailable: !enhance.available

    spacing: 10
    width: parent.width

    // Mirror the shell's selection onto the controller, exactly as
    // CutoutSection does. The controller guards no-op writes.
    onClipIdChanged: enhance.clipId = clipId

    // The section header, always drawn so the pane's shape is stable.
    Text {
        color: Theme.textSecondary
        font.bold: true
        font.pixelSize: Theme.fs
        text: "Enhance / Upscale"
    }

    // ── availability gate ────────────────────────────────────────────────
    // The runtime is a compile-time capability: when it is not built in, the
    // whole section is a disabled reason, never a dead control.
    Text {
        color: Theme.textDisabled
        font.pixelSize: Theme.fs
        text: enhance.statusText
        visible: section.unavailable
        width: parent.width
        wrapMode: Text.WordWrap
    }

    // ── factor choice + action ───────────────────────────────────────────
    // Shown whenever the runtime is available and no run owns the section.
    Column {
        spacing: 6
        visible: !section.unavailable && !section.consenting && !section.running
        width: parent.width

        Text {
            color: Theme.textSecondary
            font.pixelSize: Theme.fs
            text: "Scale"
        }
        Row {
            spacing: 4

            ToggleButton {
                checked: enhance.factor === 1
                label: "1×"

                onClicked: enhance.factor = 1
            }
            ToggleButton {
                checked: enhance.factor === 2
                label: "2×"

                onClicked: enhance.factor = 2
            }
            ToggleButton {
                checked: enhance.factor === 4
                label: "4×"

                onClicked: enhance.factor = 4
            }
        }
        Row {
            spacing: 8

            Button {
                accent: true
                enabled: section.clipId !== ""
                label: "Enhance"

                onClicked: enhance.applyEnhance()
            }
        }
    }

    // ── consent gate ─────────────────────────────────────────────────────
    // Shown before the first download of the enhance model. The four fields
    // come straight from the controller's consent properties.
    Column {
        spacing: 6
        visible: section.consenting
        width: parent.width

        Text {
            color: Theme.textSecondary
            font.pixelSize: Theme.fs
            text: "The enhance model must be downloaded before the first run."
            width: parent.width
            wrapMode: Text.WordWrap
        }
        Text {
            color: Theme.textPrimary
            font.bold: true
            font.pixelSize: Theme.fsLg
            text: enhance.consentName
        }
        Text {
            color: Theme.textSecondary
            font.family: Theme.fontTechnical
            font.pixelSize: Theme.fs
            text: enhance.consentSize + "  ·  " + enhance.consentLicense
        }
        Text {
            color: Theme.textSecondary
            elide: Text.ElideMiddle
            font.family: Theme.fontTechnical
            font.pixelSize: Theme.fsSm
            text: enhance.consentUrl
            width: parent.width
        }
        Row {
            spacing: 8

            Button {
                accent: true
                label: "Download"

                onClicked: enhance.confirmDownload()
            }
            Button {
                label: "Cancel"

                onClicked: enhance.declineDownload()
            }
        }
    }

    // ── progress ─────────────────────────────────────────────────────────
    // Shown while the worker owns the run. `running` stays true until the
    // worker confirms a cancel, so the bar keeps showing through the stop.
    Column {
        spacing: 6
        visible: section.running
        width: parent.width

        Text {
            color: Theme.textSecondary
            font.pixelSize: Theme.fs
            text: enhance.stage
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
                width: parent.width * enhance.progress
            }
        }
        Row {
            spacing: 8
            width: parent.width

            Text {
                color: Theme.textSecondary
                font.family: Theme.fontTechnical
                font.pixelSize: Theme.fsSm
                text: Math.round(enhance.progress * 100) + "%"
            }
            Button {
                label: "Cancel"

                onClicked: enhance.cancel()
            }
        }
    }

    // ── outcome ──────────────────────────────────────────────────────────
    // A refusal or a failed run: the controller's reason, in the danger style
    // the export dialog uses for its error line.
    Text {
        color: Theme.danger
        font.pixelSize: Theme.fs
        text: enhance.error
        visible: section.failed && enhance.error.length > 0
        width: parent.width
        wrapMode: Text.WordWrap
    }
    Text {
        color: Theme.textSecondary
        font.pixelSize: Theme.fs
        text: "Enhance run cancelled."
        visible: section.cancelled
        width: parent.width
    }
    Text {
        color: Theme.success
        font.pixelSize: Theme.fs
        text: "Enhancement applied."
        visible: section.done
        width: parent.width
    }
}
