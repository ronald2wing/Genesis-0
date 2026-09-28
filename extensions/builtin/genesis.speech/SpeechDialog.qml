// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The text-to-speech dialog as a contributed extension dialog (the builtin
// `genesis.speech` extension). It is the QML port of the old SpeechDialog: the
// typed text and the speaking rate, the voice picker, the consent prompt
// (model name, size, license, URL) with Download/Cancel, the progress bar with
// the current stage and a Cancel while a run is live, and the terminal error
// line in Theme.danger. A successful run closes the dialog; a refusal (empty
// text, runtime disabled) is shown in place.
//
// The dialog owns no host state. It reaches the host through the `extensions`
// context property's `call(method, paramsJson)` bridge, which returns the same
// JSON-RPC reply envelope the CLI sees:
//
//   * `ai.models`   -> {"result": {models: [{kind, model, installed, sizeBytes,
//                        license, url}], voices: [{name, index, cloned}]}}
//   * `ai.download` -> {"result": {}} (the call is the consent)
//   * `ai.status`   -> {"result": {entries: [{kind, state, progress, error}]}}
//   * `ai.run`      -> {"result": {}} or {"error": {code, message}}
//   * `ai.cancel`   -> {"result": {}}
//
// The voice picker is fed by `ai.models`'s `voices` array: a Kokoro speaker
// carries its integer id (sent as `params.voice`), a cloned voice carries
// `cloned = true` and is sent as a string name. The old dialog's "Clone voice…"
// picker has no bridge verb, so it is not offered here; already-captured cloned
// voices are still listed and selectable.
//
// The old dialog bound to the TtsController's properties directly; the bridge
// has no push channel, so the dialog polls `ai.status` on a ~4 Hz Timer while a
// download or run is live. `ai.status` collapses the controller's "Cancelled"
// into "failed", so a cancel the user asked for is remembered locally and shown
// as cancelled rather than as an error.
//
// The dialog is loaded from the installed-extension store, outside the app's
// QML directory, so it imports the app module (`GenesisApp`) for the design
// system (Theme, Panel, Button, InputField, LabelledSlider, ToggleButton) and
// reads the shared context property (`extensions`). It dismisses itself through
// `dialogBridge.close()`.

import QtQuick
import GenesisApp 1.0

Item {
    id: dialog

    // True when the user asked to cancel, so a "failed" status with no error is
    // shown as cancelled rather than as a failure.
    property bool cancelRequested: false
    readonly property bool cancelled: stage === "failed" && cancelRequested

    // The consent gate: shown when the kokoro model is not installed. The
    // Download button is the consent; the API call is the consent, so there is
    // no separate prompt.
    readonly property bool consenting: model !== null && model.installed !== true

    // The last error the host returned, shown in the danger colour. Empty when
    // the last call succeeded.
    property string errorText: ""
    readonly property bool failed: stage === "failed" && !cancelRequested

    // The kokoro model's metadata, from `ai.models`. Null until the first reply
    // lands.
    property var model: null

    // The run's read-only view, folded from `ai.status`.
    property double progress: 0.0

    // True while a download or a run is live, so the poll runs and the buttons
    // gate.
    property bool running: false

    // The speaking rate; 1.0 is the voice's natural pace.
    property double speed: 1.0
    property string stage: "idle"

    // The text to speak.
    property string text: ""

    // The selected voice, from `ai.models`'s `voices`: a Kokoro speaker's
    // integer id, or a cloned voice's name. Null until the first reply lands.
    property var voice: null

    // The selectable voices, one map per voice ({name, index, cloned}). Empty
    // until the first `ai.models` reply lands.
    property var voices: []

    // Ask the running download or run to stop. The host keeps the work live
    // until the worker confirms, so the poll continues until then.
    function cancelRun() {
        dialog.cancelRequested = true;
        if (reply.resultOf(extensions.call("ai.cancel", JSON.stringify({
                                                                           kind: "tts"
                                                                       }))) === null)
            return;
        poll();
    }

    // Dismiss the dialog through the host bridge.
    function close() {
        dialogBridge.close();
    }

    // Discover the kokoro model's metadata and the selectable voices. Called
    // once on completion; the catalogue is static, so there is no per-edit
    // refresh.
    function loadModel() {
        dialog.errorText = "";
        const result = reply.resultOf(extensions.call("ai.models", "{}"));
        if (result === null)
            return;
        const models = result.models !== undefined ? result.models : [];
        for (let i = 0; i < models.length; ++i) {
            if (models[i].kind === "tts" && models[i].model === "kokoro") {
                dialog.model = models[i];
                break;
            }
        }
        dialog.voices = result.voices !== undefined ? result.voices : [];
        dialog.voice = dialog.voices.length > 0 ? dialog.voices[0] : null;
    }

    // Fold one `ai.status` reply into the dialog's view. A malformed or refused
    // reply sets `errorText` and leaves the last good state in place, so a
    // transient failure does not blank the progress bar.
    function poll() {
        const result = reply.resultOf(extensions.call("ai.status", "{}"));
        if (result === null)
            return;
        const entries = result.entries !== undefined ? result.entries : [];
        for (let i = 0; i < entries.length; ++i) {
            if (entries[i].kind !== "tts")
                continue;
            const entry = entries[i];
            dialog.stage = entry.state !== undefined ? entry.state : "idle";
            dialog.progress = entry.progress !== undefined ? entry.progress : 0.0;
            if (entry.error !== undefined && entry.error !== "")
                dialog.errorText = entry.error;
            if (dialog.stage === "done") {
                dialog.running = false;
                dialog.close();
                return;
            }
            if (dialog.stage === "failed") {
                dialog.running = false;
                return;
            }
            if (dialog.stage === "idle" && dialog.running) {
                dialog.running = false;
                dialog.close();
                return;
            }
            return;
        }
    }

    // Accept the download implied by the consent prompt. The API call is the
    // consent, so no separate prompt; the poll then reports the install.
    function startDownload() {
        dialog.errorText = "";
        dialog.cancelRequested = false;
        if (reply.resultOf(extensions.call("ai.download", JSON.stringify({
                                                                             kind: "tts"
                                                                         }))) === null)
            return;
        dialog.running = true;
        dialog.stage = "downloading";
        dialog.progress = 0.0;
        poll();
    }

    // Start a narration run for the typed text. Refuses locally when the text
    // is empty, so the reason is visible before the call.
    function startRun() {
        dialog.errorText = "";
        dialog.cancelRequested = false;
        if (dialog.text.trim() === "") {
            dialog.errorText = "Type some text to speak.";
            return;
        }
        const params = {
            text: dialog.text,
            speed: dialog.speed
        };
        if (dialog.voice !== null)
            params.voice = dialog.voice.cloned ? dialog.voice.name : dialog.voice.index;
        if (reply.resultOf(extensions.call("ai.run", JSON.stringify({
                                                                        kind: "tts",
                                                                        params: params
                                                                    }))) === null)
            return;
        dialog.running = true;
        dialog.stage = "running";
        dialog.progress = 0.0;
        poll();
    }

    Component.onCompleted: loadModel()

    AiReply {
        id: reply

        errorTarget: dialog
    }

    // ~4 Hz while a download or run is live: the old dialog repainted on the
    // controller's progressChanged signal; the bridge is a synchronous call, so
    // a slower poll keeps the UI thread free while still reading as live.
    Timer {
        interval: 250
        repeat: true
        running: dialog.running

        onTriggered: dialog.poll()
    }
    Panel {
        id: panel

        anchors.centerIn: parent
        height: 380
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

            // ── typed text + rate + voice ────────────────────────────────
            // Shown whenever no run owns the dialog and the model is installed.
            Column {
                spacing: 6
                visible: !dialog.consenting && !dialog.running
                width: parent.width

                Text {
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fs
                    text: "Text to speak"
                }
                InputField {
                    accessibleName: "Text to speak"
                    focus: true
                    placeholder: "Type narration…"
                    text: dialog.text
                    width: parent.width

                    onTextChanged: dialog.text = text
                }
                LabelledSlider {
                    decimals: 2
                    label: "Speed"
                    max: 2.0
                    min: 0.5
                    modelValue: dialog.speed

                    onEdited: value => dialog.speed = value
                }

                // ── voice ──────────────────────────────────────────────
                // Kokoro speakers (integer id) plus the captured cloned voices
                // (selected by name), from `ai.models`'s `voices` array.
                Text {
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fs
                    text: "Voice"
                }
                Flow {
                    spacing: 6
                    width: parent.width

                    Repeater {
                        model: dialog.voices

                        ToggleButton {
                            checked: dialog.voice !== null && dialog.voice.name === modelData.name
                            label: modelData.name

                            onClicked: dialog.voice = modelData
                        }
                    }
                }
            }

            // ── consent gate ─────────────────────────────────────────────
            // Shown before the first download of the pinned model. The four
            // fields come straight from `ai.models`.
            Column {
                spacing: 6
                visible: dialog.consenting && !dialog.running
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
                    text: dialog.model ? dialog.model.model : "kokoro"
                }
                Text {
                    color: Theme.textSecondary
                    font.family: Theme.fontTechnical
                    font.pixelSize: Theme.fs
                    text: dialog.model ? (dialog.model.sizeBytes + " bytes  ·  "
                                          + dialog.model.license) : ""
                }
                Text {
                    color: Theme.textSecondary
                    elide: Text.ElideMiddle
                    font.family: Theme.fontTechnical
                    font.pixelSize: Theme.fsSm
                    text: dialog.model ? dialog.model.url : ""
                    width: parent.width
                }
            }

            // ── progress ─────────────────────────────────────────────────
            // Shown while the worker owns the run (download or synthesis).
            Column {
                spacing: 6
                visible: dialog.running
                width: parent.width

                Text {
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fs
                    text: dialog.stage === "downloading" ? "Downloading model…" : "Synthesizing…"
                }
                Rectangle {
                    Accessible.name: dialog.stage === "downloading" ? "Model download progress" :
                                                                      "Synthesis progress"
                    Accessible.role: Accessible.ProgressBar
                    Accessible.value: dialog.progress
                    color: Theme.control
                    height: 8
                    radius: 4
                    width: parent.width

                    Rectangle {
                        color: Theme.accent
                        height: parent.height
                        radius: 4
                        width: parent.width * dialog.progress
                    }
                }
                Text {
                    color: Theme.textSecondary
                    font.family: Theme.fontTechnical
                    font.pixelSize: Theme.fsSm
                    text: Math.round(dialog.progress * 100) + "%"
                }
            }

            // ── outcome ──────────────────────────────────────────────────
            Text {
                color: Theme.danger
                font.pixelSize: Theme.fs
                text: dialog.errorText
                visible: dialog.errorText !== ""
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

            // ── buttons ──────────────────────────────────────────────────
            Row {
                layoutDirection: Qt.RightToLeft
                spacing: 8
                width: parent.width

                Button {
                    accent: true
                    label: "Download"
                    visible: dialog.consenting && !dialog.running

                    onClicked: dialog.startDownload()
                }
                Button {
                    accent: true
                    enabled: dialog.text.trim() !== ""
                    label: "Speak"
                    visible: !dialog.consenting && !dialog.running

                    onClicked: dialog.startRun()
                }
                Button {
                    label: "Cancel"
                    visible: dialog.running

                    onClicked: dialog.cancelRun()
                }
                Button {
                    label: "Close"
                    visible: !dialog.running

                    onClicked: dialog.close()
                }
            }
        }
    }
}
