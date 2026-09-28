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
        Accessible.description: bar.playing ? "Pause playback" : "Start playback"
        height: bar.height
        iconOnly: true
        iconSource: bar.playing ? "qrc:/icons/pause.svg" : "qrc:/icons/play.svg"
        label: bar.playing ? "Pause" : "Play"

        onClicked: bar.toggled()
    }
    Button {
        enabled: edit.canUndo
        height: bar.height
        iconDisabledSource: "qrc:/icons/undo-disabled.svg"
        iconOnly: true
        iconSource: "qrc:/icons/undo.svg"
        label: "Undo"
        objectName: "undoButton"

        onClicked: edit.undo()
    }
    Button {
        enabled: edit.canRedo
        height: bar.height
        iconDisabledSource: "qrc:/icons/redo-disabled.svg"
        iconOnly: true
        iconSource: "qrc:/icons/redo.svg"
        label: "Redo"
        objectName: "redoButton"

        onClicked: edit.redo()
    }
    Button {
        enabled: edit.selectedClipId !== ""
        height: bar.height
        iconDisabledSource: "qrc:/icons/delete-disabled.svg"
        iconOnly: true
        iconSource: "qrc:/icons/delete.svg"
        label: "Delete"
        objectName: "deleteButton"

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
