// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The enhance overlay: a dimmed, Panel-based sheet (like CutoutDialog) that
// drives one EnhanceController run over the selected clip. The controller owns
// the whole state machine - the consent gate, the download/decode/enhance/
// encode worker, the cancel flag and the error text - and this file is pure
// presentation over its properties: it shows the consent prompt (model name,
// size, license, URL) with Download/Cancel, the progress bar with the current
// stage and a Cancel while a run is live, and the terminal error line in
// Theme.danger. A successful run closes the sheet and raises a short transient
// notice; a refusal (no clip, no picture, runtime disabled) lands in
// `enhance.error` and is shown in place.
//
// The inspector's EnhanceSection is the always-visible affordance; this sheet
// is the modal path opened from the File menu, and both read the same
// controller properties.
//
// Hand-rolled like the rest of the shell (no QtQuick.Controls): the progress
// bar is two Rectangles, buttons are Button.

import QtQuick

Rectangle {
    id: dialog

    readonly property bool cancelled: enhance.state === "Cancelled"
    readonly property bool consenting: enhance.state === "Consent"
    readonly property bool failed: enhance.state === "Failed"

    // The transient success notice, raised after a run lands Done. Empty means
    // no notice. Main.qml owns the timer that clears it.
    property string notice: ""
    readonly property bool running: enhance.state === "Running"

    // The controller's terminal states, read once so the sheet's branches stay
    // declarative. "Consent" and "Running" are the two live states; the rest
    // are terminal and show the outcome.
    readonly property bool unavailable: !enhance.available

    function close() {
        visible = false;
    }
    function open() {
        notice = "";
        visible = true;
    }

    anchors.fill: parent
    color: Theme.scrim
    visible: false

    // A run that finished while the sheet was open: close it and raise the
    // notice. A failure keeps the sheet open so the reason stays readable.
    Connections {
        function onFinished(ok, error) {
            if (ok) {
                dialog.notice = "Enhancement applied.";
                dialog.close();
            }
        }

        target: enhance
    }

    // Block input to the shell behind the dialog.
    MouseArea {
        anchors.fill: parent
    }
    Panel {
        id: panel

        anchors.centerIn: parent
        height: 300
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
                text: "Enhance / Upscale"
            }
            Rectangle {
                color: Theme.panelBorder
                height: 1
                width: parent.width
            }

            // ── availability gate ────────────────────────────────────────
            // The runtime is a compile-time capability: when it is not built
            // in, the sheet shows the reason and no controls.
            Text {
                color: Theme.textDisabled
                font.pixelSize: Theme.fs
                text: enhance.statusText
                visible: dialog.unavailable
                width: parent.width
                wrapMode: Text.WordWrap
            }

            // ── consent gate ─────────────────────────────────────────────
            // Shown before the first download of the enhance model. The four
            // fields come straight from the controller's consent properties.
            Column {
                spacing: 6
                visible: dialog.consenting
                width: parent.width

                Text {
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fs
                    text: "The enhance model must be downloaded before the " + "first run."
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
            }

            // ── progress ─────────────────────────────────────────────────
            // Shown while the worker owns the run (download then enhance then
            // encode). `running` stays true until the worker confirms a cancel,
            // so the bar keeps showing through the stop.
            Column {
                spacing: 6
                visible: dialog.running
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
                Text {
                    color: Theme.textSecondary
                    font.family: Theme.fontTechnical
                    font.pixelSize: Theme.fsSm
                    text: Math.round(enhance.progress * 100) + "%"
                }
            }

            // ── outcome ──────────────────────────────────────────────────
            // A refusal or a failed run: the controller's reason, in the
            // danger style the export dialog uses for its error line.
            Text {
                color: Theme.danger
                font.pixelSize: Theme.fs
                text: enhance.error
                visible: dialog.failed && enhance.error.length > 0
                width: parent.width
                wrapMode: Text.WordWrap
            }
            Text {
                color: Theme.textSecondary
                font.pixelSize: Theme.fs
                text: "Enhance run cancelled."
                visible: dialog.cancelled
                width: parent.width
            }

            // ── buttons ──────────────────────────────────────────────────
            Row {
                layoutDirection: Qt.RightToLeft
                spacing: 8
                width: parent.width

                Button {
                    accent: true
                    label: "Download"
                    visible: dialog.consenting

                    onClicked: enhance.confirmDownload()
                }
                Button {
                    label: "Cancel"
                    visible: dialog.consenting || dialog.running

                    onClicked: {
                        if (dialog.consenting)
                            enhance.declineDownload();
                        else
                            enhance.cancel();
                    }
                }
                Button {
                    label: "Close"
                    visible: !dialog.consenting && !dialog.running

                    onClicked: dialog.close()
                }
            }
        }
    }
}
