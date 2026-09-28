// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The cutout section of the clip inspector: the selected clip's background
// removal, driven by the `cutout` controller. It mirrors the shell's selection
// onto the controller's clipId (exactly as EffectParamsSection drives
// `effectParams`) and presents the controller's state machine in place:
//
//   unavailable -> the runtime's own reason (statusText) in the disabled style;
//   Idle        -> the subject chips (Person / Object) and Apply / Remove;
//   Consent     -> the model name/size/license/URL with Download / Cancel;
//   Running     -> a progress bar, the current stage and Cancel;
//   Failed      -> the controller's reason in Theme.danger;
//   Done        -> a short success line (the masks appear via the render path).
//
// The consent and progress states are also mirrored by CutoutDialog in
// Main.qml, which is the modal sheet the File menu opens; this inline section
// is the always-visible inspector affordance. Both read the same controller
// properties, so they can never disagree.
//
// Hand-rolled like the rest of the shell (no QtQuick.Controls): the progress
// bar is two Rectangles, buttons are Button, chips are ToggleButton.

import QtQuick

Column {
    id: section

    readonly property bool cancelled: cutout.state === "Cancelled"

    // The clip the shell has selected; the section mirrors it onto the
    // controller. Empty means nothing is selected.
    property string clipId: ""
    readonly property bool consenting: cutout.state === "Consent"
    readonly property bool done: cutout.state === "Done"
    readonly property bool failed: cutout.state === "Failed"

    // Whether the shell's stroke-painting overlay over the monitor is up,
    // mirrored from the shell so the toggle shows the live state. The toggle
    // only requests the flip (paintToggleRequested); the shell owns the state
    // and feeds it back through `paintActive`.
    property bool paintActive: false
    readonly property bool running: cutout.state === "Running"

    // The subject the user picked, held locally until Apply. "person" or
    // "object"; the controller parses it and refuses an unknown one.
    property string subject: "person"

    // The controller's state, read once so the branches below stay declarative.
    readonly property bool unavailable: !cutout.available

    signal paintToggleRequested

    spacing: 10
    width: parent.width

    // Mirror the shell's selection onto the controller, exactly as
    // EffectParamsSection does. The controller guards no-op writes.
    onClipIdChanged: cutout.clipId = clipId

    // The section header, always drawn so the pane's shape is stable.
    Text {
        color: Theme.textSecondary
        font.bold: true
        font.pixelSize: Theme.fs
        text: "Cutout / Background"
    }

    // ── manual strokes ──────────────────────────────────────────────────
    // The always-available correction path: paint strokes straight onto the
    // monitor, independent of the automatic runtime. The toggle asks the shell
    // to show the overlay (a bare stroke creates a custom cutout, so no
    // subject/model run is needed to start correcting).
    ToggleButton {
        checked: section.paintActive
        enabled: section.clipId !== ""
        label: "Paint Strokes"

        onClicked: section.paintToggleRequested()
    }

    // ── availability gate ────────────────────────────────────────────────
    // The runtime is a compile-time capability: when it is not built in, the
    // whole section is a disabled reason, never a dead control.
    Text {
        color: Theme.textDisabled
        font.pixelSize: Theme.fs
        text: cutout.statusText
        visible: section.unavailable
        width: parent.width
        wrapMode: Text.WordWrap
    }

    // ── subject choice + actions ─────────────────────────────────────────
    // Shown whenever the runtime is available and no run owns the section.
    Column {
        spacing: 6
        visible: !section.unavailable && !section.consenting && !section.running
        width: parent.width

        Text {
            color: Theme.textSecondary
            font.pixelSize: Theme.fs
            text: "Subject"
        }
        Row {
            spacing: 4

            ToggleButton {
                checked: section.subject === "person"
                label: "Person"

                onClicked: section.subject = "person"
            }
            ToggleButton {
                checked: section.subject === "object"
                label: "Object"

                onClicked: section.subject = "object"
            }
        }
        Row {
            spacing: 8

            Button {
                accent: true
                enabled: section.clipId !== ""
                label: "Apply"

                onClicked: cutout.applyCutout(section.subject)
            }
            Button {
                enabled: section.clipId !== ""
                label: "Remove"

                onClicked: cutout.clearCutout()
            }
        }
    }

    // ── consent gate ─────────────────────────────────────────────────────
    // Shown before the first download of the subject's model. The four fields
    // come straight from the controller's consent properties.
    Column {
        spacing: 6
        visible: section.consenting
        width: parent.width

        Text {
            color: Theme.textSecondary
            font.pixelSize: Theme.fs
            text: "The cutout model must be downloaded before the first run."
            width: parent.width
            wrapMode: Text.WordWrap
        }
        Text {
            color: Theme.textPrimary
            font.bold: true
            font.pixelSize: Theme.fsLg
            text: cutout.consentName
        }
        Text {
            color: Theme.textSecondary
            font.family: Theme.fontTechnical
            font.pixelSize: Theme.fs
            text: cutout.consentSize + "  ·  " + cutout.consentLicense
        }
        Text {
            color: Theme.textSecondary
            elide: Text.ElideMiddle
            font.family: Theme.fontTechnical
            font.pixelSize: Theme.fsSm
            text: cutout.consentUrl
            width: parent.width
        }
        Row {
            spacing: 8

            Button {
                accent: true
                label: "Download"

                onClicked: cutout.confirmDownload()
            }
            Button {
                label: "Cancel"

                onClicked: cutout.declineDownload()
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
            text: cutout.stage
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
                width: parent.width * cutout.progress
            }
        }
        Row {
            spacing: 8
            width: parent.width

            Text {
                color: Theme.textSecondary
                font.family: Theme.fontTechnical
                font.pixelSize: Theme.fsSm
                text: Math.round(cutout.progress * 100) + "%"
            }
            Button {
                label: "Cancel"

                onClicked: cutout.cancel()
            }
        }
    }

    // ── outcome ──────────────────────────────────────────────────────────
    // A refusal or a failed run: the controller's reason, in the danger style
    // the export dialog uses for its error line.
    Text {
        color: Theme.danger
        font.pixelSize: Theme.fs
        text: cutout.error
        visible: section.failed && cutout.error.length > 0
        width: parent.width
        wrapMode: Text.WordWrap
    }
    Text {
        color: Theme.textSecondary
        font.pixelSize: Theme.fs
        text: "Cutout run cancelled."
        visible: section.cancelled
        width: parent.width
    }
    Text {
        color: Theme.success
        font.pixelSize: Theme.fs
        text: "Cutout applied."
        visible: section.done
        width: parent.width
    }
}
