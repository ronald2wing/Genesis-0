// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The inspector's clip list: every clip on the active timeline, flattened from
// the nested tracks projection and ordered by start. Clicking a row selects
// the clip (edit.select), which the strip highlight and the clip inspector
// both follow; the edits themselves happen in the inspector pane.

import QtQuick

Panel {
    id: list

    // Flatten tracks -> [{trackId, name, kind, start, duration}], by start.
    property var clips: {
        var out = [];
        for (var t = 0; t < tracks.length; t++) {
            var tr = tracks[t];
            for (var c = 0; c < tr.clips.length; c++) {
                var cl = tr.clips[c];
                out.push({
                             id: cl.id,
                             trackId: tr.id,
                             name: cl.name,
                             kind: cl.kind,
                             start: cl.start,
                             duration: cl.duration
                         });
            }
        }
        out.sort(function (a, b) {
            return a.start - b.start;
        });
        return out;
    }
    property var tracks: []

    Column {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        Text {
            color: Theme.textPrimary
            font.bold: true
            font.pixelSize: Theme.fsXl
            text: "Clips"
        }
        Rectangle {
            color: Theme.panelBorder
            height: 1
            width: parent.width
        }
        ListView {
            clip: true
            height: parent.height - 42
            model: list.clips
            spacing: 6
            width: parent.width

            delegate: Rectangle {
                border.color: Theme.accentBorder
                border.width: edit.selectedClipId === modelData.id ? 1 : 0
                color: edit.selectedClipId === modelData.id ? Theme.selectionWash : (index % 2
                                                                                     === 0 ? Theme.rowAlt :
                                                                                             "transparent")
                height: 40
                radius: 3
                width: parent.width

                MouseArea {
                    anchors.fill: parent

                    onClicked: edit.select(modelData.id)
                }
                Column {
                    anchors.fill: parent
                    anchors.margins: 4
                    spacing: 1

                    Text {
                        color: Theme.textPrimary
                        elide: Text.ElideRight
                        font.pixelSize: Theme.fsLg
                        text: modelData.name
                        width: parent.width
                    }
                    Text {
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fs
                        text: "[" + modelData.kind + "]  " + timelineModel.fmt(modelData.start)
                              + " - " + timelineModel.fmt(modelData.start + modelData.duration)
                              + "  (" + modelData.trackId + ")"
                    }
                }
            }
        }
    }
}
