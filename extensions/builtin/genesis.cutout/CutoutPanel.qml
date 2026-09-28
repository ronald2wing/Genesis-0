// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The cutout form as an inspector panel (the builtin `genesis.cutout`
// extension). It is the QML port of the old CutoutSection/CutoutDialog,
// compacted to fit the short inspector pane (~100px): the always-available
// Paint Strokes toggle sits above the subject chips (Person / Object) and the
// Apply / Remove actions that share one row, and the consent prompt, the
// progress bar and the outcome line replace that row while a run owns the
// pane. There is no title (the tab already says "Cutout").
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
//   * `edit.apply`     -> {"result": <editor view>} or {"error": {code, message}}
//   * `ui.paint`       -> {"result": {}} (sets the paint overlay's active state)
//
// Apply starts `ai.run {kind:"cutout", clipId, params:{subject}}`; the clip id
// comes from `edit.selection`. Remove is a model-free disable: the old section
// called `cutout.clearCutout()`, which applies one `SetClipCutout` with no
// cutout, so the panel sends the same command through `edit.apply`
// (`{"op":"setClipCutout","clipId":…,"cutout":null}`) - no new API verb needed.
//
// The old section bound to the CutoutController's properties directly; the
// bridge has no push channel, so the panel polls `ai.status` on a ~4 Hz Timer
// while a download or run is live (the old section repainted on the
// controller's progressChanged signal). `ai.status` collapses the controller's
// "Cancelled" into "failed", so a cancel the user asked for is remembered
// locally and shown as cancelled rather than as an error.
//
// The panel is loaded from the installed-extension store, outside the app's
// QML directory, so it imports the app module (`GenesisApp`) for the design
// system (Theme, Button, ToggleButton) and reads the shared context properties
// (`extensions`, and `paint` for the stroke-painting overlay's state).

import QtQuick
import GenesisApp 1.0

Item {
    id: panel

    // True when the user asked to cancel, so a "failed" status with no error is
    // shown as cancelled rather than as a failure.
    property bool cancelRequested: false
    readonly property bool cancelled: stage === "failed" && cancelRequested

    // The subject's model metadata, from `ai.models`. Null until the first
    // reply lands. The consent prompt reads its name/size/license/url.
    property var consentModel: null

    // The consent gate: shown when the selected subject's model is not
    // installed. The Download button is the consent; the API call is the
    // consent, so there is no separate prompt.
    readonly property bool consenting: consentModel !== null && consentModel.installed !== true

    // The last error the host returned, shown in the danger colour. Empty when
    // the last call succeeded.
    property string errorText: ""

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

    // The subject the user picked, held locally until Apply. "person" or
    // "object"; the host parses it and refuses an unknown one.
    property string subject: "person"

    // Ask the running download or run to stop. The host keeps the work live
    // until the worker confirms, so the poll continues until then.
    function cancelRun() {
        panel.cancelRequested = true;
        if (reply.resultOf(extensions.call("ai.cancel", JSON.stringify({
                                                                           kind: "cutout"
                                                                       }))) === null)
            return;
        poll();
    }

    // Discover the selected subject's model metadata. Called on completion and
    // whenever the subject changes; the catalogue is static, so there is no
    // per-edit refresh.
    function loadModel() {
        panel.errorText = "";
        const result = reply.resultOf(extensions.call("ai.models", "{}"));
        if (result === null)
            return;
        const models = result.models !== undefined ? result.models : [];
        const wanted = panel.subject === "person" ? "rvm" : "isnet";
        for (let i = 0; i < models.length; ++i) {
            if (models[i].kind === "cutout" && models[i].model === wanted) {
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
            if (entries[i].kind !== "cutout")
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

    // Remove the selected clip's cutout: one `SetClipCutout` with no cutout,
    // applied through `edit.apply` so it is a single undo step. Refuses locally
    // when no clip is selected, so the reason is visible before the call.
    function removeCutout() {
        panel.errorText = "";
        const selection = reply.resultOf(extensions.call("edit.selection", "{}"));
        if (selection === null)
            return;
        const clipId = selection.clipId !== undefined ? selection.clipId : "";
        if (clipId === "") {
            panel.errorText = "Select a clip first.";
            return;
        }
        if (reply.resultOf(extensions.call("edit.apply", JSON.stringify({
                                                                            op: "setClipCutout",
                                                                            clipId: clipId,
                                                                            cutout: null
                                                                        }))) === null)
            return;
    }

    // Accept the download implied by the consent prompt. The API call is the
    // consent, so no separate prompt; the poll then reports the install.
    function startDownload() {
        panel.errorText = "";
        panel.cancelRequested = false;
        if (reply.resultOf(extensions.call("ai.download", JSON.stringify({
                                                                             kind: "cutout"
                                                                         }))) === null)
            return;
        panel.running = true;
        panel.stage = "downloading";
        panel.progress = 0.0;
        poll();
    }

    // Start a cutout run over the selected clip for the chosen subject. Refuses
    // locally when no clip is selected, so the reason is visible before the
    // call.
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
                                                                        kind: "cutout",
                                                                        clipId: clipId,
                                                                        params: {
                                                                            subject: panel.subject
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
    // shell, so the form is one control row and nothing else: the subject chips
    // and Apply / Remove. The consent prompt, the progress bar and the outcome
    // replace that row while a run owns the pane.
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

        // ── row 0: paint strokes ─────────────────────────────────────────
        // The always-available correction path, independent of the automatic
        // runtime: a bare stroke creates a custom cutout, so no subject/model
        // run is needed to start correcting. The toggle flips the shell's
        // stroke-painting overlay over the monitor through the `ui.paint`
        // verb; its state is the shared `paint` context property, so the
        // checked state mirrors the overlay and a deselected clip disables it.
        Row {
            spacing: 6
            width: parent.width

            ToggleButton {
                checked: paint.active
                enabled: paint.clipId !== ""
                label: "Paint Strokes"

                onClicked: {
                    extensions.call("ui.paint", JSON.stringify({
                                                                   active: !paint.active
                                                               }));
                }
            }
        }

        // ── row 1: subject chips + Apply / Remove ────────────────────────
        // Shown whenever no run owns the pane. The chips pick the subject; the
        // host refuses an unknown one.
        Row {
            id: subjectRow

            spacing: 6
            visible: !panel.consenting && !panel.running
            width: parent.width

            ToggleButton {
                id: personChip

                checked: panel.subject === "person"
                label: "Person"

                onClicked: {
                    panel.subject = "person";
                    panel.loadModel();
                }
            }
            ToggleButton {
                id: objectChip

                checked: panel.subject === "object"
                label: "Object"

                onClicked: {
                    panel.subject = "object";
                    panel.loadModel();
                }
            }
            Item {
                height: 1
                width: Math.max(0, parent.width - personChip.width - objectChip.width
                                - applyButton.width - removeButton.width - 4 * parent.spacing)
            }
            Button {
                id: applyButton

                accent: true
                label: "Apply"

                onClicked: panel.startRun()
            }
            Button {
                id: removeButton

                label: "Remove"

                onClicked: panel.removeCutout()
            }
        }

        // ── consent gate ─────────────────────────────────────────────────
        // Shown before the first download of the subject's model. The four
        // fields come straight from `ai.models`.
        Column {
            spacing: 4
            visible: panel.consenting && !panel.running
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
        // Shown while the worker owns the run (download then mask generation).
        Column {
            spacing: 4
            visible: panel.running
            width: parent.width

            Text {
                color: Theme.textSecondary
                font.pixelSize: Theme.fs
                text: panel.stage === "downloading" ? "Downloading model…" : "Segmenting…"
            }
            Rectangle {
                Accessible.name: panel.stage === "downloading" ? "Model download progress" :
                                                                 "Segmentation progress"
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
            text: "Cutout run cancelled."
            visible: panel.cancelled
            width: parent.width
        }
        Text {
            color: Theme.success
            font.pixelSize: Theme.fs
            text: "Cutout applied."
            visible: panel.stage === "done" && !panel.running
            width: parent.width
        }
    }
}
