// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The captions overlay: a dimmed, Panel-based sheet (like ExportDialog and
// SettingsDialog) that drives one AiController run over the selected clip.
// The controller owns the whole state machine - the consent gate, the
// download/extract/transcribe worker, the cancel flag and the error text - and
// this file is pure presentation over its properties: it shows the consent
// prompt (model name, size, license, URL) with Download/Cancel, the progress
// bar with the current stage and a Cancel while a run is live, and the
// terminal error line in Theme.danger. A successful run closes the sheet and
// raises a short transient notice; a refusal (no clip, no audio, runtime
// disabled) lands in `captions.error` and is shown in place.
//
// Hand-rolled like the rest of the shell (no QtQuick.Controls): the progress
// bar is two Rectangles, buttons are Button.

import QtQuick

Rectangle {
    id: dialog

    readonly property bool cancelled: captions.state === "Cancelled"
    readonly property bool consenting: captions.state === "Consent"
    readonly property bool failed: captions.state === "Failed"

    // The transient success notice, raised after a run lands Done. Empty means
    // no notice. Main.qml owns the timer that clears it.
    property string notice: ""
    readonly property bool running: captions.state === "Running"

    // The controller's terminal states, read once so the sheet's branches stay
    // declarative. "Consent" and "Running" are the two live states; the rest
    // are terminal and show the outcome.
    readonly property bool unavailable: !captions.available

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
                dialog.notice = "Captions added.";
                dialog.close();
            }
        }

        target: captions
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
                text: "Captions"
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
                text: captions.statusText
                visible: dialog.unavailable
                width: parent.width
                wrapMode: Text.WordWrap
            }

            // ── consent gate ─────────────────────────────────────────────
            // Shown before the first download of the pinned model. The four
            // fields come straight from the controller's consent properties.
            Column {
                spacing: 6
                visible: dialog.consenting
                width: parent.width

                Text {
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fs
                    text: "The caption model must be downloaded before the " + "first run."
                    width: parent.width
                    wrapMode: Text.WordWrap
                }
                Text {
                    color: Theme.textPrimary
                    font.bold: true
                    font.pixelSize: Theme.fsLg
                    text: captions.consentName
                }
                Text {
                    color: Theme.textSecondary
                    font.family: Theme.fontTechnical
                    font.pixelSize: Theme.fs
                    text: captions.consentSize + "  ·  " + captions.consentLicense
                }
                Text {
                    color: Theme.textSecondary
                    elide: Text.ElideMiddle
                    font.family: Theme.fontTechnical
                    font.pixelSize: Theme.fsSm
                    text: captions.consentUrl
                    width: parent.width
                }
            }

            // ── progress ─────────────────────────────────────────────────
            // Shown while the worker owns the run (download, extraction or
            // transcription). `running` stays true until the worker confirms a
            // cancel, so the bar keeps showing through the stop.
            Column {
                spacing: 6
                visible: dialog.running
                width: parent.width

                Text {
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fs
                    text: captions.stage
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
                        width: parent.width * captions.progress
                    }
                }
                Text {
                    color: Theme.textSecondary
                    font.family: Theme.fontTechnical
                    font.pixelSize: Theme.fsSm
                    text: Math.round(captions.progress * 100) + "%"
                }
            }

            // ── outcome ──────────────────────────────────────────────────
            // A refusal or a failed run: the controller's reason, in the
            // danger style the export dialog uses for its error line.
            Text {
                color: Theme.danger
                font.pixelSize: Theme.fs
                text: captions.error
                visible: dialog.failed && captions.error.length > 0
                width: parent.width
                wrapMode: Text.WordWrap
            }
            Text {
                color: Theme.textSecondary
                font.pixelSize: Theme.fs
                text: "Caption run cancelled."
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

                    onClicked: captions.confirmDownload()
                }
                Button {
                    label: "Cancel"
                    visible: dialog.consenting || dialog.running

                    onClicked: {
                        if (dialog.consenting)
                            captions.declineDownload();
                        else
                            captions.cancel();
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
