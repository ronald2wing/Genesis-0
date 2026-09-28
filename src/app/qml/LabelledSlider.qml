// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// A labelled slider with a live readout, extracted from ClipInspector's
// FieldSlider. `modelValue` is the projection's value (the source of truth);
// while the thumb is dragged the readout tracks `dragValue` locally and only
// commits on release, so one drag is one undo step rather than one per pixel.
// Pure presentation - the caller owns the value and decides what `edited`
// means.

import QtQuick

Column {
    id: fs

    property int decimals: 2
    property real dragValue: 0
    property bool dragging: false
    property string label: ""
    property real max: 1
    property real min: 0
    property real modelValue: 0

    signal edited(real value)

    function shown() {
        return fs.dragging ? fs.dragValue : fs.modelValue;
    }
    function step(direction) {
        const span = fs.max - fs.min;
        if (span <= 0)
            return;
        const next = Math.max(fs.min, Math.min(fs.max, fs.shown() + direction * span / 100));
        fs.edited(next);
    }
    function toValue(x) {
        var t = Math.max(0, Math.min(x, fs.track.width));
        var ratio = fs.track.width > 0 ? t / fs.track.width : 0;
        var v = fs.min + ratio * (fs.max - fs.min);
        if (fs.decimals >= 0)
            v = Math.round(v * Math.pow(10, fs.decimals)) / Math.pow(10, fs.decimals);
        return v;
    }

    // A slider to assistive tech: the label names it and the increase/decrease
    // actions step it by one percent of the range (the same granularity a
    // keyboard user expects from a slider). Qt Quick's Accessible attached
    // type has no writable `value` property, so the readout is announced
    // through the role and the step actions rather than a value binding.
    Accessible.name: label
    Accessible.role: Accessible.Slider
    spacing: 2
    width: parent.width

    Accessible.onDecreaseAction: fs.step(-1)
    Accessible.onIncreaseAction: fs.step(1)

    Row {
        width: parent.width

        Text {
            color: Theme.textSecondary
            elide: Text.ElideRight
            font.pixelSize: Theme.fs
            text: fs.label
            width: 62
        }
        Text {
            color: Theme.textPrimary
            font.family: Theme.fontTechnical
            font.pixelSize: Theme.fs
            text: fs.shown().toFixed(fs.decimals)
        }
    }
    Rectangle {
        id: track

        color: Theme.control
        height: 6
        radius: 3
        width: parent.width

        Rectangle {
            color: Theme.accent
            height: track.height
            radius: 3
            width: (fs.shown() - fs.min) / (fs.max - fs.min) * track.width
        }
        Rectangle {
            id: thumb

            anchors.verticalCenter: parent.verticalCenter
            color: Theme.textPrimary
            height: 12
            radius: 6
            width: 12
            x: (fs.shown() - fs.min) / (fs.max - fs.min) * track.width - thumb.width / 2
        }
        MouseArea {
            anchors.fill: parent

            onPositionChanged: mouse => {
                if (fs.dragging)
                    fs.dragValue = fs.toValue(mouse.x);
            }
            onPressed: mouse => {
                fs.dragging = true;
                fs.dragValue = fs.toValue(mouse.x);
            }
            onReleased: mouse => {
                fs.dragValue = fs.toValue(mouse.x);
                fs.dragging = false;
                fs.edited(fs.dragValue);
            }
        }
    }
}
