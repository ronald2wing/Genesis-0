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
// Deliberately NOT in scope for this increment: drag-from-bin-to-timeline,
// marquee selection, folders/collections, import dialogs, and waveform
// generation (no audio-decode adapter exists in the host, so Generate is a
// documented no-op and the card draws an existing .peaks cache only).

import QtQuick
import QtQuick.Dialogs

Panel {
    id: bin

    // The filtered/sorted card list, so the count label and the grid share
    // one projection rather than recomputing it apart.
    readonly property var items: mediaBin.apply(timelineModel.media, mediaBin.searchText,
                                                mediaBin.sortMode)

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
                color: Theme.textPrimary
                font.bold: true
                font.pixelSize: Theme.fsXl
                text: "Media"
            }
            Text {
                anchors.right: parent.right
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

            placeholder: "Search"
            width: parent.width

            onTextChanged: mediaBin.searchText = searchBox.text
        }
        Row {
            spacing: 6
            width: parent.width

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
                label: "Import"

                onClicked: mediaBin.sortMode = 2
            }
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

                    // A generate affordance for audio with no cache yet. A
                    // documented no-op: no audio-decode adapter exists, so it
                    // does nothing today but keeps the intent visible.
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
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    anchors.fill: parent

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
                }
            }
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
}
