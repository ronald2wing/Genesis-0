// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The transport bar: play/pause, undo/redo/delete, the timeline's name, and
// the position / duration readout as mm:ss.ff. The timeline strip owns
// seeking; this bar owns play state, edit history controls and the clock.

import QtQuick

Row {
    id: bar

    property bool playing: false
    property double position: 0.0

    signal toggled

    height: 34
    spacing: 12

    Button {
        height: bar.height
        label: bar.playing ? "Pause" : "Play"
        width: 64

        onClicked: bar.toggled()
    }
    Button {
        enabled: edit.canUndo
        height: bar.height
        label: "Undo"
        width: 56

        onClicked: edit.undo()
    }
    Button {
        enabled: edit.canRedo
        height: bar.height
        label: "Redo"
        width: 56

        onClicked: edit.redo()
    }
    Button {
        enabled: edit.selectedClipId !== ""
        height: bar.height
        label: "Delete"
        width: 56

        onClicked: edit.removeSelected()
    }
    Text {
        color: Theme.textSecondary
        font.pixelSize: Theme.fsLg
        height: bar.height
        text: timelineModel.name
        verticalAlignment: Text.AlignVCenter
    }
    Text {
        color: Theme.textPrimary
        font.family: Theme.fontTechnical
        font.pixelSize: Theme.fsInput
        height: bar.height
        text: timelineModel.fmt(bar.position) + " / " + timelineModel.fmt(timelineModel.duration)
        verticalAlignment: Text.AlignVCenter
    }
}
