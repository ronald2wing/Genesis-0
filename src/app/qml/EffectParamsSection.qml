// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The effect-parameter section of the clip inspector: for the selected clip,
// the applied picture effects (name + enabled) and, under each, one control
// per declared parameter. It is driven by the `effectParams` controller - the
// pane sets its clipId from the shell's selection (exactly as KeyframesPane
// drives `keyframes`) and re-reads effects() whenever the controller reports a
// change. Every write goes through effectParams.setParameter, which lands as
// one undoable command; the global undo/redo affordances (TransportBar, Edit
// menu) already cover it, so this section adds none of its own.
//
// Control by parameter type:
//   enum   -> a row of ToggleButton chips, one per enumValues label (the LUT
//             look names: "Teal Orange", "Vintage Fade", ...). A chip click
//             sets the enum's value.
//   bool   -> a single ToggleButton, checked when the value is non-zero.
//   int    -> LabelledSlider with step 1 and no decimals.
//   float  -> LabelledSlider honouring min/max/step/unit.
//   color  -> a swatch plus four 0..1 R/G/B/A sliders; each slider commits the
//             whole colour as one packed value (setColor).
//   point  -> two 0..1 X/Y sliders, committed together (setPoint).
//   wheel  -> X/Y puck sliders plus a master slider, committed together
//             (setWheel).
//   curve  -> a plot whose points are dragged, clicked to add and cleared to
//             reset; each release commits the whole curve (setCurve).
//
// The controller's projection is re-read into a plain JS array rather than
// bound live, matching KeyframesPane: the controller is the source of truth
// and re-reading keeps the pane's copy in step.

import QtQuick

Column {
    id: section

    // The clip the shell has selected; the section mirrors it onto the
    // controller. Empty means nothing is selected.
    property string clipId: ""

    // The controller's projection, re-read on every change it reports.
    property var effects: []

    // The refusal reason from the last edit, shown when non-empty.
    readonly property string lastError: effectParams.lastError

    function refresh() {
        effects = effectParams.effects();
    }

    spacing: 10
    width: parent.width

    // Mirror the shell's selection onto the controller, then re-read. The
    // controller guards no-op writes, so this is cheap on every selection
    // change.
    onClipIdChanged: {
        effectParams.clipId = clipId;
        refresh();
    }

    Connections {
        function onEffectsChanged() {
            section.refresh();
        }

        target: effectParams
    }

    // The section header, always drawn so the pane's shape is stable.
    Text {
        color: Theme.textSecondary
        font.bold: true
        font.pixelSize: Theme.fs
        text: "Effects"
    }
    Text {
        color: Theme.textDisabled
        font.pixelSize: Theme.fs
        text: "No effects on this clip."
        visible: section.effects.length === 0
        width: parent.width
        wrapMode: Text.WordWrap
    }

    // The refusal reason, when the last edit was refused.
    Text {
        color: Theme.danger
        font.pixelSize: Theme.fs
        text: section.lastError
        visible: section.lastError !== ""
        width: parent.width
        wrapMode: Text.WordWrap
    }
    Repeater {
        model: section.effects

        Column {
            id: effectBlock

            spacing: 6
            width: section.width

            // The effect's name and enabled state. An id the catalogue no
            // longer knows has an empty name; fall back to the raw id so the
            // link is still identifiable.
            Row {
                spacing: 6
                width: parent.width

                Text {
                    color: Theme.textPrimary
                    elide: Text.ElideRight
                    font.bold: true
                    font.pixelSize: Theme.fsMd
                    text: modelData.name !== "" ? modelData.name : modelData.id
                    width: parent.width - enabledChip.width - 6
                }
                ToggleButton {
                    id: enabledChip

                    checked: modelData.enabled
                    // The controller exposes no enable/disable verb, so this
                    // is a read-only state mark, not a control.
                    enabled: false
                    label: modelData.enabled ? "On" : "Off"
                }
            }

            // One control per declared parameter, in manifest order. The
            // Loader carries the parameter map and its owning effect's entry
            // index as its own properties, so each control reads them from
            // the Loader by id rather than walking the item tree.
            Repeater {
                model: modelData.parameters

                Loader {
                    id: paramLoader

                    property int entry: effectBlock.modelData.entry
                    property var parameter: modelData

                    sourceComponent: {
                        switch (modelData.type) {
                        case "enum":
                            return enumControl;
                        case "bool":
                            return boolControl;
                        case "int":
                            return intControl;
                        case "float":
                            return floatControl;
                        case "color":
                            return colorControl;
                        case "point":
                            return pointControl;
                        case "wheel":
                            return wheelControl;
                        case "curve":
                            return curveControl;
                        }
                        return readOnlyControl;
                    }
                    width: effectBlock.width
                }
            }
        }
    }

    // ── the per-type controls ────────────────────────────────────────────
    // Each is a Component instantiated by a Loader above; the control reads
    // `paramLoader.parameter` / `paramLoader.entry` through the Loader's id.

    Component {
        id: enumControl

        Column {
            spacing: 2
            width: parent.width

            Text {
                color: Theme.textSecondary
                font.pixelSize: Theme.fs
                text: paramLoader.parameter.label
            }
            Flow {
                spacing: 4
                width: parent.width

                Repeater {
                    model: paramLoader.parameter.labels

                    ToggleButton {
                        checked: paramLoader.parameter.value === paramLoader.parameter.values[index]
                        label: modelData

                        onClicked: effectParams.setParameter(paramLoader.entry,
                                                             paramLoader.parameter.key,
                                                             paramLoader.parameter.values[index])
                    }
                }
            }
        }
    }
    Component {
        id: boolControl

        Row {
            spacing: 6
            width: parent.width

            Text {
                color: Theme.textSecondary
                elide: Text.ElideRight
                font.pixelSize: Theme.fs
                text: paramLoader.parameter.label
                width: 62
            }
            ToggleButton {
                checked: paramLoader.parameter.value !== 0
                label: paramLoader.parameter.value !== 0 ? "On" : "Off"

                onClicked: effectParams.setParameter(paramLoader.entry, paramLoader.parameter.key,
                                                     paramLoader.parameter.value !== 0 ? 0 : 1)
            }
        }
    }
    Component {
        id: intControl

        LabelledSlider {
            decimals: 0
            label: paramLoader.parameter.label
            max: paramLoader.parameter.max
            min: paramLoader.parameter.min
            modelValue: paramLoader.parameter.value

            onEdited: v => effectParams.setParameter(paramLoader.entry, paramLoader.parameter.key,
                                                     v)
        }
    }
    Component {
        id: floatControl

        LabelledSlider {
            decimals: 2
            label: paramLoader.parameter.label
            max: paramLoader.parameter.max
            min: paramLoader.parameter.min
            modelValue: paramLoader.parameter.value

            onEdited: v => effectParams.setParameter(paramLoader.entry, paramLoader.parameter.key,
                                                     v)
        }
    }

    // color: a swatch of the packed RGBA and one slider per channel. Each
    // slider commits the whole colour, passing the other three channels back
    // from the projection, so a drag is one undo step and the channels stay
    // in step with the packed value.
    Component {
        id: colorControl

        Column {
            spacing: 2
            width: parent.width

            Row {
                spacing: 6
                width: parent.width

                Rectangle {
                    border.color: Theme.controlBorder
                    color: Qt.rgba(paramLoader.parameter.r, paramLoader.parameter.g,
                                   paramLoader.parameter.b, paramLoader.parameter.a)
                    height: 18
                    radius: Theme.radiusSmall
                    width: 62
                }
                Text {
                    color: Theme.textSecondary
                    elide: Text.ElideRight
                    font.pixelSize: Theme.fs
                    text: paramLoader.parameter.label
                    width: parent.width - 68
                }
            }
            LabelledSlider {
                decimals: 3
                label: "R"
                max: 1
                min: 0
                modelValue: paramLoader.parameter.r

                onEdited: v => effectParams.setColor(paramLoader.entry, paramLoader.parameter.key, v,
                                                     paramLoader.parameter.g,
                                                     paramLoader.parameter.b,
                                                     paramLoader.parameter.a)
            }
            LabelledSlider {
                decimals: 3
                label: "G"
                max: 1
                min: 0
                modelValue: paramLoader.parameter.g

                onEdited: v => effectParams.setColor(paramLoader.entry, paramLoader.parameter.key,
                                                     paramLoader.parameter.r, v,
                                                     paramLoader.parameter.b,
                                                     paramLoader.parameter.a)
            }
            LabelledSlider {
                decimals: 3
                label: "B"
                max: 1
                min: 0
                modelValue: paramLoader.parameter.b

                onEdited: v => effectParams.setColor(paramLoader.entry, paramLoader.parameter.key,
                                                     paramLoader.parameter.r,
                                                     paramLoader.parameter.g, v,
                                                     paramLoader.parameter.a)
            }
            LabelledSlider {
                decimals: 3
                label: "A"
                max: 1
                min: 0
                modelValue: paramLoader.parameter.a

                onEdited: v => effectParams.setColor(paramLoader.entry, paramLoader.parameter.key,
                                                     paramLoader.parameter.r,
                                                     paramLoader.parameter.g,
                                                     paramLoader.parameter.b, v)
            }
        }
    }

    // point: two sliders over the 0..1 square, committed together.
    Component {
        id: pointControl

        Column {
            spacing: 2
            width: parent.width

            Text {
                color: Theme.textSecondary
                font.pixelSize: Theme.fs
                text: paramLoader.parameter.label
            }
            LabelledSlider {
                decimals: 3
                label: "X"
                max: 1
                min: 0
                modelValue: paramLoader.parameter.x

                onEdited: v => effectParams.setPoint(paramLoader.entry, paramLoader.parameter.key, v,
                                                     paramLoader.parameter.y)
            }
            LabelledSlider {
                decimals: 3
                label: "Y"
                max: 1
                min: 0
                modelValue: paramLoader.parameter.y

                onEdited: v => effectParams.setPoint(paramLoader.entry, paramLoader.parameter.key,
                                                     paramLoader.parameter.x, v)
            }
        }
    }

    // wheel: two puck sliders over the 0..1 disc plus a master over the
    // parameter's own range, committed together.
    Component {
        id: wheelControl

        Column {
            spacing: 2
            width: parent.width

            Text {
                color: Theme.textSecondary
                font.pixelSize: Theme.fs
                text: paramLoader.parameter.label
            }
            LabelledSlider {
                decimals: 3
                label: "X"
                max: 1
                min: 0
                modelValue: paramLoader.parameter.x

                onEdited: v => effectParams.setWheel(paramLoader.entry, paramLoader.parameter.key, v,
                                                     paramLoader.parameter.y,
                                                     paramLoader.parameter.m)
            }
            LabelledSlider {
                decimals: 3
                label: "Y"
                max: 1
                min: 0
                modelValue: paramLoader.parameter.y

                onEdited: v => effectParams.setWheel(paramLoader.entry, paramLoader.parameter.key,
                                                     paramLoader.parameter.x, v,
                                                     paramLoader.parameter.m)
            }
            LabelledSlider {
                decimals: 3
                label: "Master"
                max: paramLoader.parameter.max
                min: paramLoader.parameter.min
                modelValue: paramLoader.parameter.m

                onEdited: v => effectParams.setWheel(paramLoader.entry, paramLoader.parameter.key,
                                                     paramLoader.parameter.x,
                                                     paramLoader.parameter.y, v)
            }
        }
    }

    // curve: a plot of the unit square. Click empty space to add a point,
    // drag a point to move it (committed on release), Reset to clear back to
    // the identity line. The working points are a local copy seeded from the
    // projection on completion; each release commits the whole curve as one
    // undo step and the pane rebuilds the control from the fresh projection.
    Component {
        id: curveControl

        Column {
            id: curve

            property var points: []

            function clamp01(v) {
                return Math.max(0.0, Math.min(1.0, v));
            }
            function commit() {
                effectParams.setCurve(paramLoader.entry, paramLoader.parameter.key, points);
            }

            spacing: 2
            width: parent.width

            Component.onCompleted: {
                var source = paramLoader.parameter.points;
                var copy = [];
                for (var i = 0; i < source.length; ++i)
                    copy.push({
                                  x: source[i].x,
                                  y: source[i].y
                              });
                points = copy;
            }

            Text {
                color: Theme.textSecondary
                font.pixelSize: Theme.fs
                text: paramLoader.parameter.label
            }
            Rectangle {
                id: plot

                border.color: Theme.wellBorder
                color: Theme.well
                height: 96
                radius: Theme.radiusSmall
                width: parent.width

                Canvas {
                    id: canvas

                    anchors.fill: parent

                    onPaint: {
                        const ctx = getContext("2d");
                        ctx.clearRect(0, 0, width, height);

                        // The midlines mark the unit square.
                        ctx.strokeStyle = Theme.controlBorder;
                        ctx.lineWidth = 1;
                        ctx.beginPath();
                        ctx.moveTo(width / 2, 0);
                        ctx.lineTo(width / 2, height);
                        ctx.moveTo(0, height / 2);
                        ctx.lineTo(width, height / 2);
                        ctx.stroke();

                        if (curve.points.length < 2)
                            return;

                        // The curve through the points (y is up, so flipped).
                        ctx.strokeStyle = Theme.accent;
                        ctx.lineWidth = 2;
                        ctx.beginPath();
                        ctx.moveTo(curve.points[0].x * width, (1 - curve.points[0].y) * height);
                        for (var i = 1; i < curve.points.length; ++i)
                            ctx.lineTo(curve.points[i].x * width, (1 - curve.points[i].y) * height);
                        ctx.stroke();

                        // A handle at each point.
                        ctx.fillStyle = Theme.textPrimary;
                        for (var j = 0; j < curve.points.length; ++j) {
                            ctx.beginPath();
                            ctx.arc(curve.points[j].x * width, (1 - curve.points[j].y) * height, 4,
                                    0, 2 * Math.PI);
                            ctx.fill();
                        }
                    }
                }
                MouseArea {
                    id: plotMouse

                    property int dragging: -1

                    function indexAt(px, py) {
                        var best = -1;
                        var bestDist = 12 * 12;
                        for (var i = 0; i < curve.points.length; ++i) {
                            var dx = px - curve.points[i].x * plot.width;
                            var dy = py - (1 - curve.points[i].y) * plot.height;
                            var d = dx * dx + dy * dy;
                            if (d < bestDist) {
                                bestDist = d;
                                best = i;
                            }
                        }
                        return best;
                    }

                    anchors.fill: parent

                    onPositionChanged: mouse => {
                        if (plotMouse.dragging >= 0) {
                            var p = curve.points[plotMouse.dragging];
                            p.x = curve.clamp01(mouse.x / plot.width);
                            p.y = curve.clamp01(1 - mouse.y / plot.height);
                            canvas.requestPaint();
                        }
                    }
                    onPressed: mouse => {
                        var at = plotMouse.indexAt(mouse.x, mouse.y);
                        if (at >= 0) {
                            plotMouse.dragging = at;
                        } else {
                            var added = {
                                x: curve.clamp01(mouse.x / plot.width),
                                y: curve.clamp01(1 - mouse.y / plot.height)
                            };
                            curve.points.push(added);
                            curve.points.sort(function (a, b) {
                                return a.x - b.x;
                            });
                            plotMouse.dragging = curve.points.indexOf(added);
                            canvas.requestPaint();
                        }
                    }
                    onReleased: {
                        if (plotMouse.dragging >= 0)
                            curve.commit();
                        plotMouse.dragging = -1;
                    }
                }
            }
            Row {
                spacing: 6
                width: parent.width

                Button {
                    label: "Reset"

                    onClicked: effectParams.setCurve(paramLoader.entry, paramLoader.parameter.key,
                                                     [])
                }
            }
        }
    }

    // A type this build does not map to a control: shown as a readout so the
    // declared knob is at least visible.
    Component {
        id: readOnlyControl

        Row {
            spacing: 6
            width: parent.width

            Text {
                color: Theme.textSecondary
                elide: Text.ElideRight
                font.pixelSize: Theme.fs
                text: paramLoader.parameter.label
                width: 62
            }
            Text {
                color: Theme.textDisabled
                font.family: Theme.fontTechnical
                font.pixelSize: Theme.fs
                text: paramLoader.parameter.value.toFixed(2)
            }
        }
    }
}
