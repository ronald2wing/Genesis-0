// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The text-to-speech overlay: a dimmed, Panel-based sheet (like CaptionsDialog)
// that drives one TtsController run. The controller owns the whole state
// machine - the consent gate, the download/unpack/synthesize worker, the cancel
// flag and the error text - and this file is pure presentation over its
// properties: the typed text and the speaking rate when Idle, the consent
// prompt (model name, size, license, URL) with Download/Cancel, the progress
// bar with the current stage and a Cancel while a run is live, and the
// terminal error line in Theme.danger. A successful run closes the sheet and
// raises a short transient notice; a refusal (empty text, runtime disabled)
// lands in `tts.error` and is shown in place.
//
// Hand-rolled like the rest of the shell (no QtQuick.Controls): the progress
// bar is two Rectangles, buttons are Button, the speed readout is a
// LabelledSlider.

import QtQuick
import QtQuick.Dialogs

Rectangle {
    id: dialog

    readonly property bool cancelled: tts.state === "Cancelled"
    readonly property bool consenting: tts.state === "Consent"
    readonly property bool failed: tts.state === "Failed"

    // The transient success notice, raised after a run lands Done. Empty means
    // no notice. Main.qml owns the timer that clears it.
    property string notice: ""
    readonly property bool running: tts.state === "Running"

    // The controller's terminal states, read once so the sheet's branches stay
    // declarative. "Consent" and "Running" are the two live states; "Idle" is
    // where the typed text and the rate live; the rest are terminal and show
    // the outcome.
    readonly property bool unavailable: !tts.available

    function close() {
        visible = false;
    }
    function open() {
        notice = "";
        visible = true;
        tts.refreshSpeakers();
    }

    anchors.fill: parent
    color: Theme.scrim
    visible: false

    // A run that finished while the sheet was open: close it and raise the
    // notice. A failure keeps the sheet open so the reason stays readable.
    Connections {
        function onFinished(ok, error) {
            if (ok) {
                dialog.notice = "Narration added.";
                dialog.close();
                // The model is now on disk, so the speaker count can be read.
                tts.refreshSpeakers();
            }
        }

        target: tts
    }

    // Block input to the shell behind the dialog.
    MouseArea {
        anchors.fill: parent
    }
    Panel {
        id: panel

        anchors.centerIn: parent
        height: 340
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
                text: "Speech"
            }
            Rectangle {
                color: Theme.panelBorder
                height: 1
                width: parent.width
            }

            // ── availability gate ──────────────────────────────────────────
            // The runtime is a compile-time capability: when it is not built
            // in, the sheet shows the reason and no controls.
            Text {
                color: Theme.textDisabled
                font.pixelSize: Theme.fs
                text: tts.statusText
                visible: dialog.unavailable
                width: parent.width
                wrapMode: Text.WordWrap
            }

            // ── typed text + rate ──────────────────────────────────────────
            // Shown whenever the runtime is available and no run owns the
            // sheet. The text and the rate are the controller's inputs, so a
            // refusal to speak (empty text) is the controller's to own.
            Column {
                spacing: 6
                visible: !dialog.unavailable && !dialog.consenting && !dialog.running
                width: parent.width

                Text {
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fs
                    text: "Text to speak"
                }
                InputField {
                    placeholder: "Type narration…"
                    text: tts.text
                    width: parent.width

                    onTextChanged: tts.text = text
                }
                LabelledSlider {
                    decimals: 2
                    label: "Speed"
                    max: 2.0
                    min: 0.5
                    modelValue: tts.speed

                    onEdited: value => tts.speed = value
                }

                // ── voice ──────────────────────────────────────────────
                // Kokoro speakers (once the model reports its count, else a
                // single default) plus the captured cloned voices and the
                // entry point that clones a new one.
                Text {
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fs
                    text: "Voice"
                }
                Flow {
                    spacing: 6
                    width: parent.width

                    Repeater {
                        model: tts.speakerCount > 1 ? tts.speakerCount : 1

                        ToggleButton {
                            checked: tts.clonedVoice === "" && tts.voice === index
                            enabled: tts.clonedVoice === ""
                            label: tts.speakerCount > 1 ? ("Speaker " + (index + 1)) : "Default"

                            onClicked: {
                                tts.selectClonedVoice("");
                                tts.voice = index;
                            }
                        }
                    }
                    Repeater {
                        model: tts.clonedVoices

                        Row {
                            spacing: 4

                            ToggleButton {
                                checked: tts.clonedVoice === modelData
                                label: modelData

                                onClicked: tts.selectClonedVoice(modelData)
                            }
                            Button {
                                label: "×"

                                onClicked: tts.removeClonedVoice(modelData)
                            }
                        }
                    }
                    Button {
                        label: "Clone voice…"

                        onClicked: cloneDialog.open()
                    }
                }
                Row {
                    spacing: 8
                    width: parent.width

                    Button {
                        accent: true
                        enabled: tts.text.trim() !== ""
                        label: "Speak"

                        onClicked: tts.speak()
                    }
                }
            }

            // ── consent gate ───────────────────────────────────────────────
            // Shown before the first download of the pinned model. The four
            // fields come straight from the controller's consent properties.
            Column {
                spacing: 6
                visible: dialog.consenting
                width: parent.width

                Text {
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fs
                    text: "The speech model must be downloaded before the " + "first run."
                    width: parent.width
                    wrapMode: Text.WordWrap
                }
                Text {
                    color: Theme.textPrimary
                    font.bold: true
                    font.pixelSize: Theme.fsLg
                    text: tts.consentName
                }
                Text {
                    color: Theme.textSecondary
                    font.family: Theme.fontTechnical
                    font.pixelSize: Theme.fs
                    text: tts.consentSize + "  ·  " + tts.consentLicense
                }
                Text {
                    color: Theme.textSecondary
                    elide: Text.ElideMiddle
                    font.family: Theme.fontTechnical
                    font.pixelSize: Theme.fsSm
                    text: tts.consentUrl
                    width: parent.width
                }
            }

            // ── progress ───────────────────────────────────────────────────
            // Shown while the worker owns the run (download, extraction or
            // synthesis). `running` stays true until the worker confirms a
            // cancel, so the bar keeps showing through the stop.
            Column {
                spacing: 6
                visible: dialog.running
                width: parent.width

                Text {
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fs
                    text: tts.stage
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
                        width: parent.width * tts.progress
                    }
                }
                Text {
                    color: Theme.textSecondary
                    font.family: Theme.fontTechnical
                    font.pixelSize: Theme.fsSm
                    text: Math.round(tts.progress * 100) + "%"
                }
            }

            // ── outcome ────────────────────────────────────────────────────
            // A refusal or a failed run: the controller's reason, in the
            // danger style the export dialog uses for its error line.
            Text {
                color: Theme.danger
                font.pixelSize: Theme.fs
                text: tts.error
                visible: dialog.failed && tts.error.length > 0
                width: parent.width
                wrapMode: Text.WordWrap
            }
            Text {
                color: Theme.textSecondary
                font.pixelSize: Theme.fs
                text: "Speech run cancelled."
                visible: dialog.cancelled
                width: parent.width
            }

            // ── buttons ────────────────────────────────────────────────────
            Row {
                layoutDirection: Qt.RightToLeft
                spacing: 8
                width: parent.width

                Button {
                    accent: true
                    label: "Download"
                    visible: dialog.consenting

                    onClicked: tts.confirmDownload()
                }
                Button {
                    label: "Cancel"
                    visible: dialog.consenting || dialog.running

                    onClicked: {
                        if (dialog.consenting)
                            tts.declineDownload();
                        else
                            tts.cancel();
                    }
                }
                Button {
                    label: "Close"
                    visible: !dialog.unavailable && !dialog.consenting && !dialog.running

                    onClicked: dialog.close()
                }
            }
        }
    }

    // The reference-voice picker for "Clone voice…". It hands the chosen
    // file:// URL straight to the controller, which copies it into the voice
    // root and lists it.
    FileDialog {
        id: cloneDialog

        fileMode: FileDialog.OpenFile
        nameFilters: ["Audio files (*.wav)", "All files (*)"]
        title: "Choose a voice to clone"

        onAccepted: tts.cloneVoice(selectedFile)
    }
}
