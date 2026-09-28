// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The media bin: a left-hand pane of cards, one per imported file, with a
// search box and three sort orders. Each card draws its poster (generated for
// video, the source itself for an image) or its waveform (audio), plus name,
// duration/resolution/rate/codec metadata, a missing-file badge, and the
// selection highlight the timeline strip follows. The metadata reads from the
// TimelineModel projection (timelineModel.media); the cache lookups
// (posterSource / waveformPeaks) read from the MediaBinController, which owns
// the cache paths and the off-thread generation that fills them.
//
// Drag-and-drop: the grid is a DropArea for OS file drops (import) and each
// card carries a Drag for bin-to-timeline placement. The import call is
// mediaBin.importFiles(urls) - one batch, one undo step - and the timeline
// drop is edit.addClipAt(...). Both are host invokables; the guards below keep
// the pane inert (with a status line) when a build predates them.
//
// Deliberately NOT in scope for this increment: marquee selection,
// folders/collections, and import dialogs. Waveform generation runs off the UI
// thread through MediaBinController::generateWaveform, which fills the .peaks
// cache the card then draws.

import QtQuick
import QtQuick.Dialogs

Panel {
    id: bin

    // The last drop outcome, shown under the sort row. Cleared on the next
    // successful action so a stale refusal never lingers.
    property string dropStatus: ""
    property bool dropStatusError: false

    // The filtered/sorted card list, so the count label and the grid share
    // one projection rather than recomputing it apart.
    readonly property var items: mediaBin.apply(timelineModel.media, mediaBin.searchText,
                                                mediaBin.sortMode)

    // The custom mime type a bin card carries while dragged. The timeline's
    // DropArea accepts only this, so an OS file drop never lands as a clip
    // and a card drag never lands as an import.
    readonly property string mediaMime: "application/x-genesis-media-id"

    // Places a bin item on the timeline without a drag: the beginner's "put
    // this in my movie" action. It appends to the end of the first track (the
    // top lane), so repeated clicks build a sequence in order. Returns false
    // when there is no track or the host refuses the placement.
    function addToTimeline(mediaId) {
        var tracks = timelineModel.tracks;
        if (tracks.length === 0)
            return false;
        var track = tracks[0];
        var end = 0.0;
        for (var i = 0; i < track.clips.length; ++i) {
            var clip = track.clips[i];
            end = Math.max(end, clip.start + clip.duration);
        }
        return edit.addClipAt(mediaId, track.id, end);
    }

    // Imports a drop's local files through the host, one batch. Returns the
    // number the host reports imported, or -1 when the invokable is absent.
    function importDrop(urls) {
        var split = localFiles(urls);
        if (split.files.length === 0) {
            dropStatus = split.refused > 0 ? "Only local files can be imported." : "";
            dropStatusError = split.refused > 0;
            return;
        }
        if (typeof mediaBin.importFiles !== "function") {
            dropStatus = "Import is unavailable in this build.";
            dropStatusError = true;
            return;
        }
        var imported = mediaBin.importFiles(split.files);
        if (imported < 0) {
            dropStatus = "Import failed.";
            dropStatusError = true;
            return;
        }
        var skipped = split.files.length - imported + split.refused;
        dropStatus = imported + " imported" + (skipped > 0 ? ", " + skipped + " skipped" : "")
                + ".";
        dropStatusError = false;
    }

    // The local files in a drop, in order, with non-file URLs dropped. A
    // remote URL (http, a mail attachment) has no local path to probe, so it
    // is refused rather than silently skipped.
    function localFiles(urls) {
        var files = [];
        var refused = 0;
        for (var i = 0; i < urls.length; ++i) {
            var url = urls[i].toString();
            if (url.startsWith("file:"))
                files.push(url);
            else
                refused += 1;
        }
        return {
            "files": files,
            "refused": refused
        };
    }

    // Opens the missing-media context menu over the card at (px, py), in this
    // pane's coordinates, clamped so it never spills outside the pane.
    function openActions(id, missing, placeholder, px, py) {
        actionMenu.mediaId = id;
        actionMenu.itemMissing = missing;
        actionMenu.itemPlaceholder = placeholder;
        actionMenu.x = Math.max(0, Math.min(px, width - actionMenu.width));
        actionMenu.y = Math.max(0, Math.min(py, height - actionMenu.height));
        actionMenu.visible = true;
    }

    Column {
        id: header

        anchors.left: parent.left
        anchors.margins: 12
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: 8

        Row {
            width: parent.width

            Text {
                id: mediaTitle

                color: Theme.textPrimary
                font.bold: true
                font.pixelSize: Theme.fsXl
                text: "Media"
            }
            // A spacer pushes the count to the far right: a Row lays its
            // children out itself, so the count cannot anchor to the Row's
            // right edge.
            Item {
                height: 1
                width: parent.width - mediaTitle.implicitWidth - mediaCount.implicitWidth
            }
            Text {
                id: mediaCount

                color: Theme.textSecondary
                font.pixelSize: Theme.fsMd
                text: "(" + bin.items.length + ")"
            }
        }
        Rectangle {
            color: Theme.panelBorder
            height: 1
            width: parent.width
        }
        InputField {
            id: searchBox

            accessibleName: "Search media"
            placeholder: "Search"
            width: parent.width

            onTextChanged: mediaBin.searchText = searchBox.text
        }
        Row {
            spacing: 6

            ToggleButton {
                checked: mediaBin.sortMode === 0
                label: "Name"

                onClicked: mediaBin.sortMode = 0
            }
            ToggleButton {
                checked: mediaBin.sortMode === 1
                label: "Duration"

                onClicked: mediaBin.sortMode = 1
            }
            ToggleButton {
                checked: mediaBin.sortMode === 2
                label: "Date"

                onClicked: mediaBin.sortMode = 2
            }
        }
        // The primary action, pinned to the header's right edge so it never
        // collides with the sort chips however they size.
        Button {
            id: importButton

            anchors.right: parent.right
            label: "Import…"

            onClicked: importDialog.open()
        }
        Text {
            color: bin.dropStatusError ? Theme.danger : Theme.textSecondary
            elide: Text.ElideRight
            font.pixelSize: Theme.fsXs
            text: bin.dropStatus
            visible: bin.dropStatus !== ""
            width: parent.width
        }
    }
    GridView {
        id: grid

        anchors.bottom: parent.bottom
        anchors.bottomMargin: 12
        anchors.left: parent.left
        anchors.leftMargin: 12
        anchors.right: parent.right
        anchors.rightMargin: 12
        anchors.top: header.bottom
        anchors.topMargin: 8
        cellHeight: cellWidth * 9 / 16 + 46
        cellWidth: width / 2
        clip: true
        model: bin.items

        delegate: Item {
            id: cardRoot

            readonly property real barWidth: peaks.length > 0 ? posterArea.width / peaks.length : 0

            // The card's metadata line: duration, then resolution, rate and
            // codec as the probe reported them. Absent fields drop out.
            readonly property string meta: {
                var parts = [];
                if (modelData.duration >= 0)
                    parts.push(timelineModel.fmt(modelData.duration));
                if (modelData.width > 0 && modelData.height > 0)
                    parts.push(modelData.width + "×" + modelData.height);
                if (modelData.frame_rate > 0)
                    parts.push(modelData.frame_rate.toFixed(2) + " fps");
                if (modelData.video_codec !== "")
                    parts.push(modelData.video_codec);
                if (modelData.kind === "audio" && modelData.audio_codec !== "")
                    parts.push(modelData.audio_codec);
                return parts.join(" · ");
            }

            // The waveform for an audio card, or empty when no .peaks cache
            // exists (the card then offers Generate).
            readonly property var peaks: modelData.kind === "audio" ? mediaBin.waveformPeaks(
                                                                          modelData.id,
                                                                          mediaBin.cacheVersion) :
                                                                      []

            // The card is a drag source for the timeline: once the pointer
            // moves past the threshold the MouseArea starts the drag, which
            // carries the media id under `mediaMime`. A press that never
            // crosses the threshold stays a click, so selection is unaffected.
            // The drag is started explicitly (Drag.startDrag) rather than via
            // drag.target, so the card itself never moves. The Drag attached
            // property lives on the MouseArea, not the delegate root: Qt only
            // lets startDrag() run while the item it is attached to has an
            // active drag, and that state is driven by the MouseArea.
            height: grid.cellHeight
            width: grid.cellWidth

            Rectangle {
                id: card

                anchors.fill: parent
                anchors.margins: 4
                border.color: Theme.accentBorder
                border.width: mediaBin.selectedMediaId === modelData.id ? 1 : 0
                color: mediaBin.selectedMediaId === modelData.id ? Theme.selectionWash : Theme.well
                radius: 4

                Rectangle {
                    id: posterArea

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    color: Theme.page
                    height: card.width * 9 / 16

                    // Video poster / image source.
                    Image {
                        anchors.fill: parent
                        clip: true
                        fillMode: Image.PreserveAspectCrop
                        source: mediaBin.posterSource(modelData.id, mediaBin.cacheVersion)
                        visible: modelData.kind !== "audio" && source !== ""
                    }

                    // Audio waveform, drawn as thin bars over the poster slot.
                    Item {
                        anchors.fill: parent
                        visible: modelData.kind === "audio" && peaks.length > 0

                        Repeater {
                            model: peaks

                            delegate: Rectangle {
                                color: Theme.kindAudio
                                height: (Math.max(modelData.max, 0) - Math.min(modelData.min, 0))
                                        * posterArea.height / 2
                                width: barWidth
                                x: index * barWidth
                                y: posterArea.height / 2 - Math.max(modelData.max, 0)
                                   * posterArea.height / 2
                            }
                        }
                    }

                    // A generate affordance for audio with no cache yet. It
                    // asks the controller to fill the .peaks cache off the UI
                    // thread; the card redraws when the cache lands.
                    Button {
                        anchors.centerIn: parent
                        height: 22
                        label: "Generate"
                        visible: modelData.kind === "audio" && peaks.length === 0
                        width: 74

                        onClicked: mediaBin.generateWaveform(modelData.id)
                    }

                    // The kind glyph for a card with no picture at all.
                    Text {
                        anchors.centerIn: parent
                        color: Theme.textDisabled
                        font.pixelSize: 20
                        text: modelData.kind === "audio" ? "♪" : ""
                        visible: modelData.kind === "audio" && peaks.length === 0
                    }

                    // Missing-file badge.
                    Rectangle {
                        anchors.margins: 4
                        anchors.right: parent.right
                        anchors.top: parent.top
                        color: Theme.dangerWash
                        height: 16
                        radius: 3
                        visible: modelData.missing
                        width: 42

                        Text {
                            anchors.centerIn: parent
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fsXs
                            text: "missing"
                        }
                    }
                }
                Column {
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left
                    anchors.leftMargin: 6
                    anchors.right: parent.right
                    anchors.rightMargin: 6
                    anchors.top: posterArea.bottom
                    anchors.topMargin: 4
                    spacing: 1

                    Text {
                        color: Theme.textPrimary
                        elide: Text.ElideRight
                        font.pixelSize: Theme.fs
                        text: modelData.name
                        width: parent.width
                    }
                    Text {
                        color: Theme.textSecondary
                        elide: Text.ElideRight
                        font.pixelSize: Theme.fsXs
                        text: meta
                        width: parent.width
                    }
                }
                MouseArea {
                    id: cardMouse

                    // The URL of the card grabbed at drag start, used as the
                    // drag image. Empty until the grab completes.
                    property url dragImage: ""

                    // True once the pointer has moved past the drag threshold;
                    // the Drag above follows it. A plain click never sets it,
                    // so onClicked still selects.
                    property bool dragging: false
                    property real pressX: 0
                    property real pressY: 0

                    Accessible.description:
                        "Select; drag to the timeline; right-click for relink and placeholder actions"
                    Accessible.name: modelData.name
                    Accessible.role: Accessible.ListItem

                    // The drag source. Attached to the MouseArea so its drag
                    // state is active when the drag begins; the key must match
                    // bin.mediaMime and mimeData carries the id the drop reads.
                    // Drag.active is bound to `dragging`: a MouseArea with no
                    // drag.target never activates on its own, so the binding
                    // flips it active the moment the pointer crosses the
                    // threshold. The card stays put while the drag carries the
                    // media id.
                    //
                    // Drag.Automatic is required: the default (Drag.Internal)
                    // moves the source item inside its own parent, so the drag
                    // ghost is clipped to the GridView viewport and can never
                    // reach the timeline. Automatic starts a real window-level
                    // drag-and-drop that any DropArea in the scene receives.
                    Drag.active: dragging
                    Drag.dragType: Drag.Automatic
                    Drag.hotSpot: Qt.point(mouseX, mouseY)
                    // The drag image is the card itself, grabbed at drag start.
                    // A static imageSource binding is unreliable: the poster
                    // pixmap loads asynchronously, so a drag begun before it is
                    // ready shows no image at all. grabToImage always yields the
                    // card the user is looking at.
                    Drag.imageSource: dragImage
                    Drag.keys: [bin.mediaMime]
                    Drag.mimeData: {
                        "application/x-genesis-media-id": modelData.id
                    }
                    Drag.supportedActions: Qt.CopyAction
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    anchors.fill: parent
                    objectName: "cardMouse"

                    Accessible.onPressAction: mediaBin.select(modelData.id)
                    // The drag ends on drop or cancel; clear `dragging` so the
                    // Drag.active binding does not re-assert true and leave the
                    // drag stuck active (which would swallow the next drop).
                    // Restore the grid's scrolling here too: a release over a
                    // DropArea may not reach onReleased.
                    Drag.onDragFinished: {
                        dragging = false;
                        grid.interactive = true;
                    }
                    onCanceled: {
                        grid.interactive = true;
                        Drag.cancel();
                    }
                    onClicked: function (mouse) {
                        mediaBin.select(modelData.id);
                        if (mouse.button === Qt.RightButton) {
                            const at = mapToItem(bin, mouse.x, mouse.y);
                            bin.openActions(modelData.id, modelData.missing, mediaBin.isPlaceholder(
                                                modelData.id), at.x, at.y);
                        } else {
                            actionMenu.visible = false;
                        }
                    }
                    onPositionChanged: mouse => {
                        if (!dragging && (Math.abs(mouse.x - pressX) > 8 || Math.abs(mouse.y
                                                                                     - pressY)
                                          > 8)) {
                            // Flipping `dragging` activates Drag.active, which
                            // starts the drag; no explicit startDrag() call is
                            // needed (and would double-start). The drag image
                            // was grabbed on press, so it is ready by now.
                            dragging = true;
                        }
                    }
                    // The card lives in a GridView (a Flickable). Freezing the
                    // view's scrolling while the pointer is down lets the Drag
                    // become active; preventStealing would instead block the
                    // Flickable from ever entering drag state, so startDrag
                    // reports "drag must be active".
                    onPressed: mouse => {
                        grid.interactive = false;
                        dragging = false;
                        dragImage = "";
                        pressX = mouse.x;
                        pressY = mouse.y;
                        // Grab the card now, before the drag threshold is
                        // crossed, so the drag image is ready when the drag
                        // starts. The grab is async and needs a render pass;
                        // the press-to-drag interval gives it one.
                        cardRoot.grabToImage(function (result) {
                            dragImage = result.url;
                        });
                    }
                    onReleased: {
                        grid.interactive = true;
                        // A release over a DropArea is the drop itself; leave
                        // `dragging` alone so the Drag.active binding stays true
                        // until the drop is delivered. Drag.onDragFinished
                        // clears it once the drag actually ends. A release that
                        // never crossed the threshold is a click, and `dragging`
                        // is already false.
                    }
                }
                // The beginner's "put this in my movie" action: one click
                // appends the item to the end of the first track, no drag
                // required. Sits over the poster's lower-right so it never
                // collides with the name/meta column. Declared after the
                // MouseArea so it stacks on top and receives its clicks.
                Button {
                    anchors.bottom: posterArea.bottom
                    anchors.bottomMargin: 4
                    anchors.right: parent.right
                    anchors.rightMargin: 4
                    label: "Add"
                    objectName: "addButton"

                    onClicked: bin.addToTimeline(modelData.id)
                }
            }
        }
    }

    // OS file drops land here: the whole grid is one drop target, so a drop
    // anywhere over the cards imports. Only local files are accepted (the
    // importDrop guard refuses a remote URL with the error status); the
    // highlight is the only feedback while a drag hovers.
    DropArea {
        id: gridDrop

        anchors.fill: grid

        // No `keys` filter: an OS drag carries no Drag.keys, so a filter here
        // would reject every external drop. importDrop refuses a non-local URL
        // itself, which is the guard that matters.
        onDropped: drop => bin.importDrop(drop.urls)

        Rectangle {
            anchors.fill: parent
            border.color: Theme.accentBorder
            border.width: 2
            color: Theme.accentSoft
            radius: Theme.radius
            visible: gridDrop.containsDrag
        }
    }

    // Empty-state guidance: a beginner who has imported nothing sees what to do
    // instead of a blank pane. It sits over the grid and disappears the moment
    // the first item lands.
    Column {
        anchors.centerIn: grid
        spacing: 8
        visible: bin.items.length === 0

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            color: Theme.textSecondary
            font.pixelSize: Theme.fsLg
            text: "No media yet"
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            color: Theme.textDisabled
            font.pixelSize: Theme.fsMd
            text: "Drag files here, or click Import…"
        }
    }

    // The missing-media recovery menu, opened by a right-click on a card.
    // "Relink…" targets a missing file, "Fill slot…" a template slot (which
    // then becomes ordinary media), and the placeholder toggle flips the
    // item's slot state. Each action is one undoable command over `mediaBin`.
    Rectangle {
        id: actionMenu

        property bool itemMissing: false
        property bool itemPlaceholder: false
        property string mediaId: ""

        border.color: Theme.panelBorder
        border.width: 1
        color: Theme.panel
        height: actions.implicitHeight + 8
        radius: Theme.radius
        visible: false
        width: 150
        z: 100

        Column {
            id: actions

            anchors.left: parent.left
            anchors.margins: 4
            anchors.right: parent.right
            anchors.top: parent.top
            spacing: 2

            Button {
                enabled: actionMenu.itemMissing
                label: "Relink…"
                width: parent.width

                onClicked: {
                    actionMenu.visible = false;
                    fileDialog.begin(actionMenu.mediaId, false);
                }
            }
            Button {
                enabled: actionMenu.itemPlaceholder
                label: "Fill slot…"
                width: parent.width

                onClicked: {
                    actionMenu.visible = false;
                    fileDialog.begin(actionMenu.mediaId, true);
                }
            }
            Button {
                label: actionMenu.itemPlaceholder ? "Clear placeholder" : "Set placeholder"
                width: parent.width

                onClicked: {
                    mediaBin.setMediaPlaceholder(actionMenu.mediaId, !actionMenu.itemPlaceholder);
                    actionMenu.visible = false;
                }
            }
        }
    }

    // The one file picker both "Relink…" and "Fill slot…" use. It hands the
    // chosen file:// URL straight to the controller, which normalises it to a
    // local path; `filling` picks the command.
    FileDialog {
        id: fileDialog

        property bool filling: false
        property string mediaId: ""

        function begin(id, isFill) {
            mediaId = id;
            filling = isFill;
            open();
        }

        fileMode: FileDialog.OpenFile
        title: filling ? "Fill slot" : "Relink media"

        onAccepted: {
            if (filling)
                mediaBin.fillSlot(mediaId, selectedFile);
            else
                mediaBin.relinkMedia(mediaId, selectedFile);
        }
    }

    // The Import button's picker: multi-select, so a beginner can bring in a
    // whole shoot at once. It reuses the same import path as an OS drop
    // (importDrop), so the status line and undo behaviour are identical.
    FileDialog {
        id: importDialog

        fileMode: FileDialog.OpenFiles
        title: "Import media"

        onAccepted: bin.importDrop(selectedFiles)
    }
}
