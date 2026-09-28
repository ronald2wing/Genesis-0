// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The stroke-painting overlay: a transparent layer over the monitor that turns
// the selected clip's background removal into a hand-paintable mask. It is
// shown by the shell (Main.qml) only while the inspector's "Paint Strokes"
// toggle is on and a clip is selected, and floats a small tool panel over the
// live frame. Each completed stroke commits as one undo step through
// EditController.addCutoutStroke, so the mask corrections ride the same
// command layer as every other edit.
//
// Coordinates are source fractions: a point is `(mouse.x / width,
// mouse.y / height)`, so a stroke survives an output-size change, a crop or a
// flip exactly as the model's Stroke points do. The smart brush and smart
// eraser read the frame under the stroke at the playhead (`at = position`);
// the plain brush and eraser paint a disc, so they carry no instant (`at < 0`).
//
// Hand-rolled like the rest of the shell (no QtQuick.Controls): buttons are
// ToggleButton (the four-brush picker), the size slider is LabelledSlider, the
// live stroke preview is a Canvas, and the painting surface is a MouseArea.

import QtQuick

Rectangle {
    id: overlay

    property real brushSize: 0.05

    // The clip being painted onto; empty means nothing is selected (the shell
    // hides the overlay then anyway).
    property string clipId: ""

    // The stroke in flight: a flat [x0,y0,x1,y1,...] list of source fractions,
    // committed on release.
    property var points: []

    // The playhead position in seconds: the source instant a smart brush reads
    // the frame under the stroke from. The plain brushes ignore it.
    property double position: 0.0

    // The smart brushes read the picture, so they record the source instant;
    // the plain ones paint a disc and leave it absent.
    readonly property bool smart: tool === 0 || tool === 2

    // Which brush paints, by BrushTool ordinal (0 SmartBrush, 1 Brush,
    // 2 SmartEraser, 3 Eraser), and how wide, as a fraction of the frame's
    // width (the model clamps it into 0.005..=0.5).
    property int tool: 0

    // The picker's model: label and BrushTool ordinal, in declaration order.
    readonly property var tools: [
        {
            label: "Smart Brush",
            ordinal: 0
        },
        {
            label: "Brush",
            ordinal: 1
        },
        {
            label: "Smart Eraser",
            ordinal: 2
        },
        {
            label: "Eraser",
            ordinal: 3
        }
    ]

    // Commits the accumulated stroke as one undo step and clears it. A stroke
    // needs at least two points to draw a segment, so a bare tap is dropped
    // rather than recorded as a dot.
    function commit() {
        if (clipId === "" || points.length < 4) {
            points = [];
            return;
        }
        edit.addCutoutStroke(clipId, tool, brushSize, points, smart ? position : -1.0);
        points = [];
    }

    color: "transparent"

    // Discard a half-drawn stroke when the target or visibility changes.
    onClipIdChanged: points = []
    onVisibleChanged: {
        if (!visible)
            points = [];
    }

    // ── the painting surface ────────────────────────────────────────────
    // The whole frame is paintable; the tool panel floats above it (z: 1) and
    // swallows its own clicks so painting never starts under a button.

    Canvas {
        id: preview

        anchors.fill: parent
        z: 0

        onPaint: {
            const ctx = getContext("2d");
            ctx.clearRect(0, 0, width, height);
            if (points.length < 2)
                return;
            ctx.strokeStyle = Theme.accent;
            ctx.lineWidth = brushSize * width;
            ctx.lineCap = "round";
            ctx.lineJoin = "round";
            ctx.beginPath();
            ctx.moveTo(points[0] * width, points[1] * height);
            for (var i = 2; i + 1 < points.length; i += 2)
                ctx.lineTo(points[i] * width, points[i + 1] * height);
            ctx.stroke();
        }
    }
    MouseArea {
        id: paint

        Accessible.description: "Drag to paint a mask stroke over the frame"
        Accessible.name: "Stroke painting surface"
        Accessible.role: Accessible.Canvas
        anchors.fill: parent
        z: 0

        onPositionChanged: mouse => {
            if (paint.pressed) {
                points.push(mouse.x / paint.width, mouse.y / paint.height);
                preview.requestPaint();
            }
        }
        onPressed: mouse => {
            points = [mouse.x / paint.width, mouse.y / paint.height];
            preview.requestPaint();
        }
        onReleased: mouse => {
            preview.requestPaint();
            overlay.commit();
        }
    }

    // ── tool panel ──────────────────────────────────────────────────────
    // The brush picker and size slider, floating top-left above the frame.

    Panel {
        id: toolPanel

        anchors.left: parent.left
        anchors.margins: Theme.pad
        anchors.top: parent.top
        radius: Theme.radiusLarge
        width: 340
        z: 1

        // Swallow clicks on the panel's own ground so painting never starts
        // underneath it.
        MouseArea {
            anchors.fill: parent
        }
        Column {
            anchors.fill: parent
            anchors.margins: Theme.pad
            spacing: Theme.gap

            Text {
                color: Theme.textPrimary
                font.bold: true
                font.pixelSize: Theme.fs
                text: "Paint strokes"
            }
            Row {
                spacing: 4

                Repeater {
                    model: overlay.tools

                    ToggleButton {
                        checked: overlay.tool === modelData.ordinal
                        label: modelData.label

                        onClicked: overlay.tool = modelData.ordinal
                    }
                }
            }
            LabelledSlider {
                decimals: 3
                label: "Size"
                max: 0.5
                min: 0.005
                modelValue: overlay.brushSize

                onEdited: v => overlay.brushSize = v
            }
        }
    }
}
