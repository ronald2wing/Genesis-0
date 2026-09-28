// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The timeline view: the active timeline's tracks as horizontal lanes, its
// clips as colour-coded rectangles scaled to the lane width, a playhead line
// that follows the session position, and - new this increment - a per-lane
// header (name, mute, visible) and the timeline tool toggles (snap, ripple).
// Clicking empty strip seeks.
//
// Editing lives here only as intent: a clip body drag asks the EditController
// to move it (beginMove/updateMove/endMove), an edge drag asks it to trim
// (beginTrim/updateTrim/endTrim), and the controller owns the semantics. The
// only math on this side is pixels <-> seconds for the drag. The selection
// highlight follows edit.selectedClipId. The track header and tool toggles
// read state from timelineModel and write through edit - never into the model
// - so every toggle is undoable. A bin card dropped on the lanes places a clip
// through edit.addClipAt: the lane under the cursor picks the track and x maps
// to seconds, with a drop indicator drawn while the drag hovers. Deliberately
// NOT in scope for this increment: multi-select, snap guides drawn on the
// strip, ripple-truncate UI polish.

import QtQuick

Item {
    id: strip

    // Wide enough for the id plus the Vis and Mute toggles at their natural
    // widths (the toggles alone need ~86px); a narrower column clipped "Mute".
    readonly property int headerWidth: 180
    readonly property int laneGap: 4
    readonly property int laneHeight: 26

    // The custom mime type a bin card carries while dragged. Must match
    // MediaBin.mediaMime; the DropArea below accepts only this, so an OS file
    // drop never lands as a clip.
    readonly property string mediaMime: "application/x-genesis-media-id"

    // The playhead, seconds on the host's rational clock. Polled upstream.
    property double position: 0.0

    // The last timeline action's outcome (a bin drop or a cut), shown in the
    // toolbar. Cleared on the next successful action so a stale message never
    // lingers.
    property string status: ""
    property bool statusError: false
    readonly property int toolbarHeight: 26
    readonly property int trackCount: timelineModel.tracks.length > 0 ? timelineModel.tracks.length :
                                                                        1

    signal seekRequested(real seconds)
    // Emitted by the toolbar's Split button; the shell runs the same
    // "split" action the Edit menu and Ctrl+K use.
    signal splitRequested

    // kind -> colour, read from the theme so the strip and the bin card marks
    // can never drift apart. The value still lives here only as a lookup; the
    // colours are Theme's.
    function kindColor(kind) {
        switch (kind) {
        case "video":
            return Theme.kindVideo;
        case "audio":
            return Theme.kindAudio;
        case "image":
            return Theme.kindImage;
        case "text":
            return Theme.kindText;
        case "layer":
            return Theme.kindLayer;
        }
        return Theme.kindFallback;
    }

    height: toolbarHeight + 4 + trackCount * (laneHeight + laneGap) + 8

    Panel {
        anchors.fill: parent
    }
    Column {
        anchors.fill: parent
        anchors.margins: 4
        spacing: 4

        // The timeline tools. Snap pulls a dragged clip edge onto a nearby
        // clip edge, the playhead or the timeline start within a small pixel
        // radius, falling back to the frame grid (EditController::updateMove);
        // ripple makes a trim close the gap behind it
        // (EditController::updateTrim). Both are UI state the controller
        // consults, not edits of their own.
        Row {
            height: strip.toolbarHeight
            spacing: 6

            ToggleButton {
                checked: edit.snapEnabled
                iconCheckedSource: "qrc:/icons/snap-checked.svg"
                iconOnly: true
                iconSource: "qrc:/icons/snap.svg"
                label: "Snap"

                onClicked: edit.snapEnabled = !edit.snapEnabled
            }
            ToggleButton {
                checked: edit.rippleEnabled
                iconCheckedSource: "qrc:/icons/ripple-checked.svg"
                iconOnly: true
                iconSource: "qrc:/icons/ripple.svg"
                label: "Ripple"

                onClicked: edit.rippleEnabled = !edit.rippleEnabled
            }
            // The beginner's cut: split the selected clip at the playhead. The
            // same action the Edit menu's "Split at Playhead" (Ctrl+K) runs;
            // surfaced here as a button so it is discoverable without the menu.
            // Disabled until a clip is selected, since there is nothing to cut.
            Button {
                enabled: edit.selectedClipId !== ""
                iconOnly: true
                iconSource: "qrc:/icons/split.svg"
                label: "Split"

                onClicked: strip.splitRequested()
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                color: strip.statusError ? Theme.danger : Theme.textSecondary
                elide: Text.ElideRight
                font.pixelSize: Theme.fsXs
                text: strip.status
                visible: strip.status !== ""
                width: parent.width - x
            }
        }

        // Headers beside the lanes, sharing one track list so the rows never
        // drift apart from the lanes they label.
        Row {
            height: parent.height - strip.toolbarHeight - 4
            spacing: 4
            width: parent.width

            // Per-lane header: the lane's id as its name, plus mute and
            // visible toggles. State reads from the model; clicks write
            // through SetTrackFlag (edit.setTrackFlag).
            Column {
                id: headers

                spacing: strip.laneGap
                width: strip.headerWidth

                Repeater {
                    model: timelineModel.tracks

                    delegate: Rectangle {
                        border.color: Theme.wellBorder
                        color: Theme.well
                        height: strip.laneHeight
                        radius: Theme.radiusSmall
                        width: headers.width

                        Row {
                            anchors.fill: parent
                            anchors.leftMargin: 6
                            anchors.rightMargin: 6
                            spacing: 4

                            Text {
                                color: Theme.textSecondary
                                elide: Text.ElideRight
                                font.pixelSize: Theme.fs
                                height: parent.height
                                text: modelData.id
                                verticalAlignment: Text.AlignVCenter
                                // Reserve the two toggles' natural width plus
                                // the row spacing so the id elides instead of
                                // pushing "Mute" past the header's edge. Each
                                // toggle is 54px wide (label + padding); the
                                // two spacings are 4px each.
                                width: parent.width - 116
                            }
                            ToggleButton {
                                anchors.verticalCenter: parent.verticalCenter
                                checked: modelData.visible
                                label: "Vis"
                                objectName: "visToggle"

                                onClicked: edit.setTrackFlag(modelData.id, 0, !modelData.visible)
                            }
                            ToggleButton {
                                anchors.verticalCenter: parent.verticalCenter
                                checked: modelData.muted
                                label: "Mute"
                                objectName: "muteToggle"

                                onClicked: edit.setTrackFlag(modelData.id, 1, !modelData.muted)
                            }
                        }
                    }
                }
            }

            // The lanes themselves, in the room the headers leave.
            Item {
                id: lanesArea

                height: parent.height
                width: parent.width - strip.headerWidth - 4

                // The seek surface sits UNDER the lanes so a press on a clip
                // reaches the clip's own MouseArea (move/trim); a press on empty
                // lane space falls through to here and moves the playhead. It is
                // scoped to the lanes area so it never covers the track headers,
                // whose toggles must receive their own clicks.
                MouseArea {
                    Accessible.description: "Click to move the playhead"
                    Accessible.name: "Timeline seek"
                    Accessible.role: Accessible.Slider
                    anchors.fill: parent

                    onClicked: mouse => {
                        if (timelineModel.duration > 0)
                            seekRequested(mouse.x / lanesArea.width * timelineModel.duration);
                    }
                }
                Column {
                    id: lanes

                    anchors.fill: parent
                    clip: true
                    spacing: strip.laneGap

                    Repeater {
                        model: timelineModel.tracks

                        delegate: Rectangle {
                            id: lane

                            border.color: Theme.controlBorder
                            // A raised track surface, one step above the panel
                            // ground, so a clip reads as sitting IN a lane
                            // rather than floating on the pane.
                            color: Theme.control
                            height: strip.laneHeight
                            radius: Theme.radiusSmall
                            width: lanes.width

                            Repeater {
                                model: modelData.clips

                                delegate: Rectangle {
                                    id: clip

                                    readonly property real baseWidth: Math.max(
                                                                          timelineModel.duration
                                                                          > 0 ? modelData.duration
                                                                                / timelineModel.duration
                                                                                * lane.width : 0, 2)

                                    // Seconds -> pixels from the model. The
                                    // controller defers the model refresh to
                                    // gesture end, so these stay fixed while a
                                    // drag is live and the offsets below move
                                    // the clip locally.
                                    readonly property real baseX: timelineModel.duration > 0
                                                                  ? modelData.start
                                                                    / timelineModel.duration
                                                                    * lane.width : 0

                                    // Drag state. "" = idle; "move" drags the
                                    // body; "trimStart"/"trimEnd" drag one
                                    // edge.
                                    property string dragMode: ""
                                    property real dragOffset: 0
                                    property real grabX: 0
                                    property real trimOffset: 0

                                    border.color: Theme.textPrimary
                                    border.width: edit.selectedClipId === modelData.id ? 1 : 0
                                    color: strip.kindColor(modelData.kind)
                                    height: lane.height
                                    opacity: 0.88
                                    radius: 2
                                    width: dragMode === "trimStart" ? Math.max(baseWidth
                                                                               - trimOffset, 2) :
                                                                      dragMode === "trimEnd"
                                                                      ? Math.max(baseWidth
                                                                                 + trimOffset, 2) :
                                                                        baseWidth
                                    x: dragMode === "trimStart" ? baseX + trimOffset : baseX
                                                                  + dragOffset

                                    // A soft amber wash lights every clip whose
                                    // media is selected in the bin, so a bin
                                    // selection reads across the strip without
                                    // stealing the edit-selection highlight.
                                    Rectangle {
                                        anchors.fill: parent
                                        color: Theme.amberWash
                                        opacity: mediaBin.selectedMediaId !== ""
                                                 && mediaBin.selectedMediaId === modelData.media_id
                                                 ? 0.18 : 0
                                        radius: 2
                                        visible: opacity > 0
                                    }
                                    Text {
                                        anchors.left: parent.left
                                        anchors.leftMargin: 4
                                        anchors.verticalCenter: parent.verticalCenter
                                        color: Theme.clipInk
                                        elide: Text.ElideRight
                                        font.bold: true
                                        font.pixelSize: Theme.fsSm
                                        text: modelData.name
                                        width: parent.width - 8
                                    }
                                    MouseArea {
                                        Accessible.description: "Drag to move; drag an edge to trim"
                                        Accessible.name: modelData.name
                                        Accessible.role: Accessible.ListItem
                                        acceptedButtons: Qt.LeftButton
                                        anchors.fill: parent
                                        objectName: "clipMouse"

                                        onCanceled: {
                                            if (clip.dragMode === "move")
                                                edit.endMove();
                                            else if (clip.dragMode !== "")
                                                edit.endTrim();
                                            clip.dragMode = "";
                                            clip.dragOffset = 0;
                                            clip.trimOffset = 0;
                                        }
                                        onPositionChanged: mouse => {
                                            const laneX = clip.mapToItem(lane, mouse.x, mouse.y).x;
                                            const dx = laneX - clip.grabX;
                                            const secsPerPx = timelineModel.duration > 0
                                                  ? timelineModel.duration / lane.width : 0;
                                            if (clip.dragMode === "move") {
                                                clip.dragOffset = dx;
                                                edit.updateMove(modelData.id, (clip.baseX + dx)
                                                                * secsPerPx, strip.position,
                                                                secsPerPx);
                                            } else if (clip.dragMode === "trimStart") {
                                                clip.trimOffset = dx;
                                                edit.updateTrim(modelData.id, 0, dx * secsPerPx);
                                            } else if (clip.dragMode === "trimEnd") {
                                                clip.trimOffset = dx;
                                                edit.updateTrim(modelData.id, 1, dx * secsPerPx);
                                            }
                                        }
                                        onPressed: mouse => {
                                            // The pointer in lane coordinates,
                                            // which stay fixed while the clip
                                            // moves under it.
                                            clip.grabX = clip.mapToItem(lane, mouse.x, mouse.y).x;
                                            const edgePx = 6;
                                            edit.select(modelData.id);
                                            if (mouse.x <= edgePx) {
                                                clip.dragMode = "trimStart";
                                                clip.trimOffset = 0;
                                                edit.beginTrim(modelData.id, 0);
                                            } else if (mouse.x >= clip.width - edgePx) {
                                                clip.dragMode = "trimEnd";
                                                clip.trimOffset = 0;
                                                edit.beginTrim(modelData.id, 1);
                                            } else {
                                                clip.dragMode = "move";
                                                clip.dragOffset = 0;
                                                edit.beginMove(modelData.id);
                                            }
                                        }
                                        onReleased: {
                                            if (clip.dragMode === "move")
                                                edit.endMove();
                                            else if (clip.dragMode === "trimStart" || clip.dragMode
                                                     === "trimEnd")
                                                edit.endTrim();
                                            clip.dragMode = "";
                                            clip.dragOffset = 0;
                                            clip.trimOffset = 0;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // Playhead, clamped so it stays visible at the timeline's end.
                Rectangle {
                    color: Theme.textPrimary
                    height: lanesArea.height
                    width: 1
                    x: timelineModel.duration > 0 ? Math.min(position / timelineModel.duration, 1.0)
                                                    * lanesArea.width : 0
                }

                // A bin card dropped on the lanes places a clip at the drop
                // point: the lane under the cursor picks the track (tracks are
                // untyped, so the user's aim is the target), and x maps to
                // seconds through the same duration/lane-width ratio the clips
                // use. addClipAt owns the snap and the undo step; this side
                // only reports the outcome.
                DropArea {
                    id: laneDrop

                    // The lane index under a y in lanesArea coordinates, or -1
                    // when the point is past the last lane.
                    function laneAt(y) {
                        const pitch = strip.laneHeight + strip.laneGap;
                        const index = Math.floor(y / pitch);
                        return index >= 0 && index < timelineModel.tracks.length ? index : -1;
                    }

                    anchors.fill: parent
                    keys: [strip.mediaMime]
                    objectName: "laneDrop"

                    onDropped: drop => {
                        const index = laneAt(drop.y);
                        if (index < 0) {
                            strip.status = "Drop on a track to place the clip.";
                            strip.statusError = true;
                            return;
                        }
                        const mediaId = drop.getDataAsString(strip.mediaMime);
                        const trackId = timelineModel.tracks[index].id;
                        // An empty timeline has duration 0, so the x-to-time
                        // mapping would divide by zero. That is the common case
                        // (the first clip dropped in), so place it at the start
                        // instead of refusing the drop.
                        const seconds = timelineModel.duration > 0 ? drop.x / lanesArea.width
                                                                     * timelineModel.duration : 0;
                        if (edit.addClipAt(mediaId, trackId, seconds)) {
                            strip.status = "Placed on " + trackId + ".";
                            strip.statusError = false;
                        } else {
                            strip.status = "Could not place the clip.";
                            strip.statusError = true;
                        }
                    }

                    // The drop indicator: a vertical line at the pointer and a
                    // wash over the target lane, so the placement reads before
                    // the button is released.
                    Rectangle {
                        color: Theme.accent
                        height: parent.height
                        visible: laneDrop.containsDrag
                        width: 2
                        x: laneDrop.drag.x
                    }
                    Rectangle {
                        border.color: Theme.accentBorder
                        border.width: 1
                        color: Theme.accentSoft
                        height: strip.laneHeight
                        radius: Theme.radiusSmall
                        visible: laneDrop.containsDrag && laneDrop.laneAt(laneDrop.drag.y) >= 0
                        width: parent.width
                        y: laneDrop.laneAt(laneDrop.drag.y) * (strip.laneHeight + strip.laneGap)
                    }
                }

                // Empty-state guidance: with no clip on any track, a beginner
                // sees how to fill the timeline instead of bare lanes. It
                // disappears the moment the first clip lands.
                Text {
                    anchors.centerIn: parent
                    color: Theme.textDisabled
                    font.pixelSize: Theme.fsMd
                    text: "Drag media here, or click Add in the bin"
                    visible: {
                        for (var i = 0; i < timelineModel.tracks.length; ++i) {
                            if (timelineModel.tracks[i].clips.length > 0)
                                return false;
                        }
                        return true;
                    }
                }
            }
        }
    }
}
