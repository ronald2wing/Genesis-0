// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The keyframes pane: for the selected clip, the list of its keyed
// properties; for the selected property, a curve view plotting the host's own
// sampler (KeyframesController::sample) so the drawn curve cannot disagree
// with playback, the keys as draggable points, the playhead marker, and
// controls to add a key at the playhead, remove the selected key, clear the
// property, and set the selected key's ease. Every write goes through the
// KeyframesController, which lands it as one undoable command; a key drag maps
// to beginKeyMove/updateKeyMove/endKeyMove so one drag is one undo step.
//
// R4: this pane is pure model/view - it reads the controller's copies
// (QString/double) and writes intent; no media work happens here, on or off
// the UI thread. The engine reload that follows a key edit is the shell's
// existing projectEdited path, unchanged.
//
// Deliberately NOT in scope: the bezier editor handles for a "custom" ease
// (the four presets are editable here; a hand-drawn ease shows but cannot be
// reshaped), multi-property overlay (one property's curve at a time), and
// i18n. Starting a brand-new key on a property that is not yet keyed is also
// out of scope: the controller exposes only keyedProperties(), so this list is
// the properties that already carry keys.

import QtQuick

Panel {
    id: pane

    property string activeProperty: ""

    // The four presets, name for the chip and value for the controller. The
    // controller's ease names ("linear"/"in"/"out"/"inOut") cross the bridge;
    // the display name is the pane's.
    property var easePresets: [
        {
            name: "Linear",
            value: "linear"
        },
        {
            name: "In",
            value: "in"
        },
        {
            name: "Out",
            value: "out"
        },
        {
            name: "In·Out",
            value: "inOut"
        }
    ]

    // Whether there is a clip and at least one keyed property to show.
    readonly property bool hasContent: selectedClip && keyedProps.length > 0

    // The controller's own projections, re-read on every change it reports.
    // Plain JS arrays rather than live bindings: the controller is the source
    // of truth, and re-reading here keeps the pane's copies in step.
    property var keyedProps: []
    property var keys: []

    // Where the playhead sits inside the selected clip, 0..=1 - the unit a
    // key's `at` uses, so the marker and "add key at the playhead" line up.
    property double playheadInClip: {
        if (selectedClip && selectedClip.duration > 0)
            return Math.max(0, Math.min(1, (position - selectedClip.start)
                                        / selectedClip.duration));


        return 0;
    }
    property double position: 0.0
    property var samples: []

    // The selected clip, looked up in the projection; null when none.
    property var selectedClip: {
        if (edit.selectedClipId === "")
            return null;
        for (var t = 0; t < tracks.length; t++) {
            var tr = tracks[t];
            for (var c = 0; c < tr.clips.length; c++) {
                if (tr.clips[c].id === edit.selectedClipId)
                    return tr.clips[c];
            }
        }
        return null;
    }
    // The `at` of the key the Remove / ease controls act on; -1 for none.
    property double selectedKeyAt: -1

    // The shell feeds the projection and the playhead; selection comes from
    // edit.selectedClipId, exactly as ClipInspector reads it.
    property var tracks: []

    function addKeyAtPlayhead() {
        if (activeProperty === "")
            return;
        const t = playheadInClip;
        const sv = keyframes.sample(activeProperty, t, t, 2);
        const v = sv.length > 0 ? sv[0].value : defaultForKey();
        keyframes.addKey(activeProperty, t, v, "linear");
        selectedKeyAt = t;
    }
    function defaultForKey() {
        const r = propertyRange(activeProperty);
        return r.min + (r.max - r.min) / 2;
    }
    function fmt(v) {
        if (Math.abs(v) >= 100)
            return v.toFixed(0);
        if (Math.abs(v) >= 10)
            return v.toFixed(1);
        return v.toFixed(2);
    }
    function hasProperty(name) {
        for (var i = 0; i < keyedProps.length; i++)
            if (keyedProps[i].property === name)
                return true;
        return false;
    }

    // The nearest key to a press point, within a grab radius in pixels; -1
    // when the press landed on empty plot.
    function hitKey(x, y) {
        let best = -1;
        let bestD = 12;
        for (let i = 0; i < keys.length; i++) {
            const dx = x - plot.xOf(keys[i].at);
            const dy = y - plot.yOf(keys[i].value);
            const d = Math.sqrt(dx * dx + dy * dy);
            if (d < bestD) {
                bestD = d;
                best = i;
            }
        }
        return best;
    }

    // The value range a property's curve is drawn over, matching the model's
    // clamps (Clip::tidy). Volume has no model ceiling, so the plot gives it
    // a display one; rotation is drawn over one turn even though the model
    // permits a spin past ±180.
    function propertyRange(name) {
        switch (name) {
        case "scale":
            return {
                min: 0.05,
                max: 8.0
            };
        case "offsetX":
            return {
                min: -3.0,
                max: 3.0
            };
        case "offsetY":
            return {
                min: -3.0,
                max: 3.0
            };
        case "rotation":
            return {
                min: -180.0,
                max: 180.0
            };
        case "opacity":
            return {
                min: 0.0,
                max: 1.0
            };
        case "volume":
            return {
                min: 0.0,
                max: 2.0
            };
        }
        return {
            min: 0.0,
            max: 1.0
        };
    }

    // Keep the selected key pointing at a key that still exists: a drag moves
    // it, a remove clears it, a clip change drops it.
    function reconcileSelection() {
        if (selectedKeyAt < 0)
            return;
        for (var i = 0; i < keys.length; i++) {
            if (Math.abs(keys[i].at - selectedKeyAt) < 0.001) {
                selectedKeyAt = keys[i].at;
                return;
            }
        }
        selectedKeyAt = -1;
    }

    // Re-read the controller's projection after any change it (or an edit via
    // the toolbar's undo/redo) reports.
    function refresh() {
        keyedProps = keyframes.keyedProperties();
        if (keyedProps.length === 0) {
            activeProperty = "";
        } else if (!hasProperty(activeProperty)) {
            activeProperty = keyedProps[0].property;
        }
        reload();
    }

    // Re-read keys + samples for the active property and repaint the curve.
    function reload() {
        keys = activeProperty !== "" ? keyframes.keysFor(activeProperty) : [];
        samples = activeProperty !== "" ? keyframes.sample(activeProperty, 0, 1, 64) : [];
        reconcileSelection();
        curve.requestPaint();
    }
    function selectKey(at) {
        selectedKeyAt = at;
        curve.requestPaint();
    }
    function selectProperty(name) {
        if (activeProperty === name)
            return;
        activeProperty = name;
        selectedKeyAt = -1;
        reload();
    }
    function selectedKey() {
        for (var i = 0; i < keys.length; i++)
            if (Math.abs(keys[i].at - selectedKeyAt) < 0.001)
                return keys[i];
        return null;
    }
    function syncPlayhead() {
        keyframes.setPlayheadPosition(playheadInClip);
    }

    onPositionChanged: syncPlayhead()

    // Drive the controller's clip id and playhead from the shell's selection
    // and clock. The controller guards no-op writes, so calling these on
    // every position tick is cheap.
    onSelectedClipChanged: {
        keyframes.setClipId(selectedClip ? selectedClip.id : "");
        syncPlayhead();
    }

    Connections {
        function onPropertiesChanged() {
            pane.refresh();
        }

        target: keyframes
    }
    Connections {
        // Undo/redo and structural edits run through the EditController, so
        // its projectEdited is the one signal that covers "the project changed
        // through some other path"; re-read here so the pane never shows keys
        // that an undo just removed.
        function onProjectEdited() {
            pane.refresh();
        }

        target: edit
    }

    // A small labelled button is now Button (src/app/qml/Button.qml); the
    // KeyAction this pane used to roll by hand is that same surface.

    Item {
        anchors.fill: parent
        anchors.margins: 12

        Text {
            id: title

            anchors.top: parent.top
            color: Theme.textPrimary
            font.bold: true
            font.pixelSize: Theme.fsXl
            text: "Keyframes"
        }
        Rectangle {
            id: divider

            anchors.top: title.bottom
            anchors.topMargin: 8
            color: Theme.panelBorder
            height: 1
            width: parent.width
        }
        Text {
            anchors.top: divider.bottom
            anchors.topMargin: 8
            color: Theme.textDisabled
            font.pixelSize: Theme.fsMd
            text: "Select a clip to edit its keyframes."
            visible: !pane.selectedClip
            width: parent.width
            wrapMode: Text.WordWrap
        }
        Text {
            anchors.top: divider.bottom
            anchors.topMargin: 8
            color: Theme.textDisabled
            font.pixelSize: Theme.fsMd
            text: "This clip has no keyed properties."
            visible: pane.selectedClip && pane.keyedProps.length === 0
            width: parent.width
            wrapMode: Text.WordWrap
        }
        Flow {
            id: chips

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: divider.bottom
            anchors.topMargin: 8
            spacing: 6
            visible: pane.hasContent

            Repeater {
                model: pane.keyedProps

                ToggleButton {
                    checked: pane.activeProperty === modelData.property
                    label: modelData.property

                    onClicked: pane.selectProperty(modelData.property)
                }
            }
        }

        // The curve: the host's sampled ride as a polyline, the keys as
        // draggable diamonds, and the playhead marker over it. It fills the
        // space between the property chips and the controls, so no height is
        // guessed and the plot never competes with a Column's layout pass.
        Item {
            id: plot

            readonly property double pad: 10

            function atOf(x) {
                return Math.max(0, Math.min(1, (x - pad) / (plot.width - 2 * pad)));
            }
            function range() {
                return pane.propertyRange(pane.activeProperty);
            }
            function span() {
                const r = range();
                return (r.max - r.min) <= 0 ? 1 : (r.max - r.min);
            }
            function valueOf(y) {
                const r = range();
                const v = r.min + (plot.height - pad - y) / (plot.height - 2 * pad) * span();
                return Math.max(r.min, Math.min(r.max, v));
            }
            function xOf(at) {
                return pad + at * (plot.width - 2 * pad);
            }
            function yOf(v) {
                return plot.height - pad - (v - range().min) / span() * (plot.height - 2 * pad);
            }

            anchors.bottom: controls.top
            anchors.bottomMargin: 8
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: chips.bottom
            anchors.topMargin: 8
            visible: pane.hasContent

            Rectangle {
                anchors.fill: parent
                color: Theme.well
                radius: Theme.radiusSmall
            }
            Canvas {
                id: curve

                anchors.fill: parent

                onPaint: {
                    const ctx = getContext("2d");
                    const w = width, h = height;
                    const pad = plot.pad;

                    ctx.clearRect(0, 0, w, h);

                    // Grid, quarters each way.
                    ctx.strokeStyle = Theme.panelBorder;
                    ctx.lineWidth = 1;
                    for (let g = 0; g <= 4; g++) {
                        const gx = pad + g * (w - 2 * pad) / 4;
                        const gy = pad + g * (h - 2 * pad) / 4;
                        ctx.beginPath();
                        ctx.moveTo(gx, pad);
                        ctx.lineTo(gx, h - pad);
                        ctx.stroke();
                        ctx.beginPath();
                        ctx.moveTo(pad, gy);
                        ctx.lineTo(w - pad, gy);
                        ctx.stroke();
                    }

                    // The ride, from the host's own sampler.
                    if (pane.samples.length >= 2) {
                        ctx.strokeStyle = Theme.accent;
                        ctx.lineWidth = 2;
                        ctx.beginPath();
                        for (let i = 0; i < pane.samples.length; i++) {
                            const x = plot.xOf(pane.samples[i].at);
                            const y = plot.yOf(pane.samples[i].value);
                            if (i === 0)
                                ctx.moveTo(x, y);
                            else
                                ctx.lineTo(x, y);
                        }
                        ctx.stroke();
                    }

                    // The keys as diamonds, the selected one filled light.
                    for (let k = 0; k < pane.keys.length; k++) {
                        const kx = plot.xOf(pane.keys[k].at);
                        const ky = plot.yOf(pane.keys[k].value);
                        const selected = Math.abs(pane.keys[k].at - pane.selectedKeyAt) < 0.001;
                        ctx.beginPath();
                        ctx.moveTo(kx, ky - 5);
                        ctx.lineTo(kx + 5, ky);
                        ctx.lineTo(kx, ky + 5);
                        ctx.lineTo(kx - 5, ky);
                        ctx.closePath();
                        ctx.fillStyle = selected ? Theme.textPrimary : Theme.accentBorder;
                        ctx.fill();
                        ctx.strokeStyle = Theme.textPrimary;
                        ctx.lineWidth = 1;
                        ctx.stroke();
                    }

                    // The property's name and the selected key, top-left.
                    ctx.fillStyle = Theme.textSecondary;
                    ctx.font = "11px monospace";
                    let label = pane.activeProperty;
                    const sel = pane.selectedKey();
                    if (sel)
                        label += "  @" + pane.fmt(sel.at) + "  " + pane.fmt(sel.value) + "  "
                                + sel.ease;
                    ctx.fillText(label, pad + 2, pad + 12);
                }
            }

            // Playhead marker, driven by the pane's own clip-local clock.
            Rectangle {
                color: Theme.textPrimary
                height: plot.height
                opacity: 0.9
                width: 1
                x: plot.xOf(pane.playheadInClip)
                y: 0
            }
            MouseArea {
                id: curveMouse

                property bool dragging: false

                acceptedButtons: Qt.LeftButton
                anchors.fill: parent

                onCanceled: {
                    if (dragging) {
                        keyframes.endKeyMove();
                        dragging = false;
                    }
                }
                onPositionChanged: mouse => {
                    if (!dragging)
                        return;
                    const newAt = plot.atOf(mouse.x);
                    const newValue = plot.valueOf(mouse.y);
                    pane.selectKey(newAt);
                    keyframes.updateKeyMove(pane.activeProperty, newAt, newValue);
                }
                onPressed: mouse => {
                    if (pane.activeProperty === "")
                        return;
                    const hit = pane.hitKey(mouse.x, mouse.y);
                    if (hit >= 0) {
                        // Open the move gesture on the grabbed key; a click
                        // selects it, a drag moves it, one undo step.
                        const k = pane.keys[hit];
                        dragging = keyframes.beginKeyMove(pane.activeProperty, k.at);
                        if (dragging)
                            pane.selectKey(k.at);
                    } else {
                        // Empty plot: move the playhead there.
                        const t = plot.atOf(mouse.x);
                        controller.seek(pane.selectedClip.start + t * pane.selectedClip.duration);
                    }
                }
                onReleased: {
                    if (dragging) {
                        keyframes.endKeyMove();
                        dragging = false;
                    }
                }
            }
        }
        Flow {
            id: controls

            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 6
            visible: pane.hasContent

            Button {
                enabled: pane.activeProperty !== ""
                label: "Add Key"

                onClicked: pane.addKeyAtPlayhead()
            }
            Button {
                enabled: pane.selectedKey() !== null
                label: "Remove"

                onClicked: keyframes.removeKey(pane.activeProperty, pane.selectedKeyAt)
            }
            Button {
                enabled: pane.keys.length > 0
                label: "Clear"

                onClicked: keyframes.clearKeys(pane.activeProperty)
            }
            Repeater {
                model: pane.easePresets

                ToggleButton {
                    checked: pane.selectedKey() !== null && pane.selectedKey().ease
                             === modelData.value

                    enabled: pane.selectedKey() !== null
                    label: modelData.name

                    onClicked: keyframes.setKeyEase(pane.activeProperty, pane.selectedKeyAt,
                                                    modelData.value)
                }
            }
        }
    }
}
