// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The enhance form as an inspector panel (the builtin `genesis.enhance`
// extension). It is the QML port of the old EnhanceSection/EnhanceDialog,
// compacted to fit the short inspector pane (~100px): one control row holds
// the 1×/2×/4× scale chips and the Enhance action, and the consent prompt, the
// progress bar and the outcome line replace it while a run owns the pane.
// There is no title (the tab already says "Enhance").
//
// The panel owns no host state. It reaches the host through the `extensions`
// context property's `call(method, paramsJson)` bridge, which returns the same
// JSON-RPC reply envelope the CLI sees:
//
//   * `ai.models`      -> {"result": {models: [{kind, model, installed,
//                           sizeBytes, license, url}], voices: [...]}}
//   * `ai.download`    -> {"result": {}} (the call is the consent)
//   * `ai.status`      -> {"result": {entries: [{kind, state, progress, error}]}}
//   * `ai.run`         -> {"result": {}} or {"error": {code, message}}
//   * `ai.cancel`      -> {"result": {}}
//   * `edit.selection` -> {"result": {clipId}}
//
// Enhance starts `ai.run {kind:"enhance", clipId, params:{factor}}`; the clip
// id comes from `edit.selection` and the factor from the panel's 1×/2×/4×
// scale chips (the old section set the controller's `factor` directly; the
// `ai.run` verb now carries it in `params.factor`, defaulting to 2×).
//
// The old section bound to the EnhanceController's properties directly; the
// bridge has no push channel, so the panel polls `ai.status` on a ~4 Hz Timer
// while a download or run is live (the old section repainted on the
// controller's progressChanged signal). `ai.status` collapses the controller's
// "Cancelled" into "failed", so a cancel the user asked for is remembered
// locally and shown as cancelled rather than as an error.
//
// The panel is loaded from the installed-extension store, outside the app's
// QML directory, so it imports the app module (`GenesisApp`) for the design
// system (Theme, Button, ToggleButton) and reads the shared context property
// (`extensions`).

import QtQuick
import GenesisApp 1.0

Item {
    id: panel

    // True when the user asked to cancel, so a "failed" status with no error is
    // shown as cancelled rather than as a failure.
    property bool cancelRequested: false
    readonly property bool cancelled: stage === "failed" && cancelRequested

    // The enhance model's metadata, from `ai.models`. Null until the first
    // reply lands. The consent prompt reads its name/size/license/url.
    property var consentModel: null

    // The consent gate: shown when the pinned model is not installed. The
    // Download button is the consent; the API call is the consent, so there is
    // no separate prompt.
    readonly property bool consenting: consentModel !== null && consentModel.installed !== true

    // The last error the host returned, shown in the danger colour. Empty when
    // the last call succeeded.
    property string errorText: ""

    // The upscale factor the user picked, held locally until Enhance. 1, 2 or
    // 4; the host parses it and refuses an unsupported one.
    property int factor: 2

    // The runtime is a compile-time capability the API does not report; a
    // refusal from `ai.run` lands in `errorText` instead.
    readonly property bool failed: stage === "failed" && !cancelRequested

    // The run's read-only view, folded from `ai.status`. `progress` is 0..1;
    // `stage` is the host's state word ("downloading"/"running"/"done"/
    // "failed"/"idle").
    property double progress: 0.0

    // True while a download or a run is live, so the poll runs and the buttons
    // gate.
    property bool running: false
    property string stage: "idle"

    // Ask the running download or run to stop. The host keeps the work live
    // until the worker confirms, so the poll continues until then.
    function cancelRun() {
        panel.cancelRequested = true;
        if (reply.resultOf(extensions.call("ai.cancel", JSON.stringify({
                                                                           kind: "enhance"
                                                                       }))) === null)
            return;
        poll();
    }

    // Discover the pinned enhance model's metadata. Called once on completion;
    // the catalogue is static, so there is no per-edit refresh.
    function loadModel() {
        panel.errorText = "";
        const result = reply.resultOf(extensions.call("ai.models", "{}"));
        if (result === null)
            return;
        const models = result.models !== undefined ? result.models : [];
        for (let i = 0; i < models.length; ++i) {
            if (models[i].kind === "enhance" && models[i].model === "real-esrgan") {
                panel.consentModel = models[i];
                return;
            }
        }
        panel.consentModel = null;
    }

    // Fold one `ai.status` reply into the panel's view. A malformed or refused
    // reply sets `errorText` and leaves the last good state in place, so a
    // transient failure does not blank the progress bar.
    function poll() {
        const result = reply.resultOf(extensions.call("ai.status", "{}"));
        if (result === null)
            return;
        const entries = result.entries !== undefined ? result.entries : [];
        for (let i = 0; i < entries.length; ++i) {
            if (entries[i].kind !== "enhance")
                continue;
            const entry = entries[i];
            panel.stage = entry.state !== undefined ? entry.state : "idle";
            panel.progress = entry.progress !== undefined ? entry.progress : 0.0;
            if (entry.error !== undefined && entry.error !== "")
                panel.errorText = entry.error;
            if (panel.stage === "done" || panel.stage === "failed") {
                panel.running = false;
                // A finished download changes the model's installed flag, so
                // re-read the catalogue; otherwise the consent prompt would
                // stay up and ask to download a model that is already on disk.
                if (panel.stage === "done")
                    panel.loadModel();
                return;
            }
            // "idle" after a live run means the worker finished without a
            // terminal word; treat it as done.
            if (panel.stage === "idle" && panel.running) {
                panel.running = false;
                return;
            }
            return;
        }
    }

    // Accept the download implied by the consent prompt. The API call is the
    // consent, so no separate prompt; the poll then reports the install.
    function startDownload() {
        panel.errorText = "";
        panel.cancelRequested = false;
        if (reply.resultOf(extensions.call("ai.download", JSON.stringify({
                                                                             kind: "enhance"
                                                                         }))) === null)
            return;
        panel.running = true;
        panel.stage = "downloading";
        panel.progress = 0.0;
        poll();
    }

    // Start an enhance run over the selected clip. Refuses locally when no clip
    // is selected, so the reason is visible before the call.
    function startRun() {
        panel.errorText = "";
        panel.cancelRequested = false;
        const selection = reply.resultOf(extensions.call("edit.selection", "{}"));
        if (selection === null)
            return;
        const clipId = selection.clipId !== undefined ? selection.clipId : "";
        if (clipId === "") {
            panel.errorText = "Select a clip first.";
            return;
        }
        if (reply.resultOf(extensions.call("ai.run", JSON.stringify({
                                                                        kind: "enhance",
                                                                        clipId: clipId,
                                                                        params: {
                                                                            factor: panel.factor
                                                                        }
                                                                    }))) === null)
            return;
        panel.running = true;
        panel.stage = "running";
        panel.progress = 0.0;
        poll();
    }

    Component.onCompleted: loadModel()

    AiReply {
        id: reply

        errorTarget: panel
    }

    // ~4 Hz while a download or run is live: the old section repainted on the
    // controller's progressChanged signal; the bridge is a synchronous call, so
    // a slower poll keeps the UI thread free while still reading as live.
    Timer {
        interval: 250
        repeat: true
        running: panel.running && panel.visible

        onTriggered: panel.poll()
    }

    // ── the compact form ─────────────────────────────────────────────────
    // The inspector pane's panel slot is only ~100px tall in the 1280x760
    // shell, so the form is one control row and nothing else: the 1×/2×/4×
    // scale chips and the Enhance action. The consent prompt, the progress bar
    // and the outcome replace that row while a run owns the pane.
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

        // ── row 1: scale chips + the Enhance action ──────────────────────
        // Shown whenever no run owns the pane. The chips pick the upscale
        // factor (1×/2×/4×, the host refuses an unsupported one); the Enhance
        // button runs over the selected clip.
        Row {
            id: actionRow

            spacing: 6
            visible: !panel.consenting && !panel.running
            width: parent.width

            ToggleButton {
                id: oneChip

                checked: panel.factor === 1
                label: "1×"

                onClicked: panel.factor = 1
            }
            ToggleButton {
                id: twoChip

                checked: panel.factor === 2
                label: "2×"

                onClicked: panel.factor = 2
            }
            ToggleButton {
                id: fourChip

                checked: panel.factor === 4
                label: "4×"

                onClicked: panel.factor = 4
            }
            Item {
                height: 1
                width: Math.max(0, parent.width - oneChip.width - twoChip.width - fourChip.width
                                - enhanceButton.width - 4 * parent.spacing)
            }
            Button {
                id: enhanceButton

                accent: true
                label: "Enhance"

                onClicked: panel.startRun()
            }
        }

        // ── consent gate ─────────────────────────────────────────────────
        // Shown before the first download of the pinned model. The four fields
        // come straight from `ai.models`.
        Column {
            spacing: 4
            visible: panel.consenting && !panel.running
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
                text: panel.consentModel ? panel.consentModel.model : ""
            }
            Text {
                color: Theme.textSecondary
                font.family: Theme.fontTechnical
                font.pixelSize: Theme.fs
                text: panel.consentModel ? (panel.consentModel.sizeBytes + " bytes  ·  "
                                            + panel.consentModel.license) : ""
            }
            Row {
                spacing: 8

                Button {
                    accent: true
                    label: "Download"

                    onClicked: panel.startDownload()
                }
                Button {
                    label: "Cancel"

                    onClicked: panel.loadModel()
                }
            }
        }

        // ── progress ─────────────────────────────────────────────────────
        // Shown while the worker owns the run (download then enhance/encode).
        Column {
            spacing: 4
            visible: panel.running
            width: parent.width

            Text {
                color: Theme.textSecondary
                font.pixelSize: Theme.fs
                text: panel.stage === "downloading" ? "Downloading model…" : "Enhancing…"
            }
            Rectangle {
                Accessible.name: panel.stage === "downloading" ? "Model download progress" :
                                                                 "Enhancement progress"
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
            Row {
                spacing: 8
                width: parent.width

                Text {
                    color: Theme.textSecondary
                    font.family: Theme.fontTechnical
                    font.pixelSize: Theme.fsSm
                    text: Math.round(panel.progress * 100) + "%"
                }
                Button {
                    label: "Cancel"

                    onClicked: panel.cancelRun()
                }
            }
        }

        // ── outcome ──────────────────────────────────────────────────────
        Text {
            color: Theme.textSecondary
            font.pixelSize: Theme.fs
            text: "Enhance run cancelled."
            visible: panel.cancelled
            width: parent.width
        }
        Text {
            color: Theme.success
            font.pixelSize: Theme.fs
            text: "Enhancement applied."
            visible: panel.stage === "done" && !panel.running
            width: parent.width
        }
    }
}
