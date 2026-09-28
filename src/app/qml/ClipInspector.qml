// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The clip inspector: the properties of the selected clip (name, transform,
// opacity, speed, fades) as editable controls, plus the applied effect chain
// and its parameters (EffectParamsSection). It is driven by the selection
// (edit.selectedClipId) and reads the selected clip's values straight from the
// timeline projection; every write goes through the EditController so each
// adjustment is one undoable command and the strip/list refresh off the
// model's rebuild. Controls are hand-rolled QtQuick (no QtQuick.Controls in
// this shell), so the sliders below are LabelledSlider (theme token + track +
// thumb) and the name field is a InputField.
//
// The effect-parameter section is driven by the `effectParams` controller
// rather than the projection: the section mirrors the selection onto its
// clipId and re-reads the controller's effects() projection, exactly as
// KeyframesPane drives `keyframes`.
//
// Deliberately NOT in scope for this increment: keyframes (its own pane),
// scopes, and the media bin - those panes follow the same select-and-edit
// pattern once their commands exist.

import QtQuick

Panel {
    id: inspector

    // A slider's upper bound for the fade fields: the clip's own length, so a
    // fade cannot be longer than the clip it sits on (the command floors at
    // zero and the UI keeps it under the clip).
    readonly property real fadeMax: selectedClip ? Math.max(selectedClip.duration, 0.5) : 1.0

    // The extension slot contributions for this surface: every `extensions.slots`
    // entry whose `point` is "clip.inspector", already ordered by the host
    // (priority ascending). Rendered below the built-in controls.
    readonly property var inspectorSlots: {
        const all = extensions.slots;
        const out = [];
        for (let i = 0; i < all.length; ++i) {
            if (all[i].point === "clip.inspector")
                out.push(all[i]);
        }
        return out;
    }

    // The clip the selection names, looked up in the projection. Null when
    // nothing is selected, so the pane collapses to a hint.
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
    property var tracks: []

    // Absolute transform commit: the six placement fields travel together so
    // SetClipTransform sees a consistent picture, not the one field the user
    // happened to touch.
    function applyTransform(scale, offsetX, offsetY, rotation, stretchX, stretchY) {
        if (selectedClip)
            edit.setClipTransform(selectedClip.id, scale, offsetX, offsetY, rotation, stretchX,
                                  stretchY);
    }

    Column {
        anchors.fill: parent
        anchors.margins: Theme.pad
        spacing: Theme.spacing

        Text {
            color: Theme.textPrimary
            font.bold: true
            font.pixelSize: Theme.fsXl
            text: "Inspector"
        }
        Rectangle {
            color: Theme.panelBorder
            height: 1
            width: parent.width
        }
        Text {
            color: Theme.textDisabled
            font.pixelSize: Theme.fsMd
            text: "Select a clip to edit its properties."
            visible: !inspector.selectedClip
            width: parent.width
            wrapMode: Text.WordWrap
        }
        Flickable {
            clip: true
            contentHeight: fields.height
            contentWidth: parent.width
            height: parent.height - 42
            visible: inspector.selectedClip !== null
            width: parent.width

            Column {
                id: fields

                spacing: 10
                width: parent.width

                // Name
                Column {
                    spacing: 2
                    width: parent.width

                    Text {
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fs
                        text: "Name"
                    }
                    InputField {
                        id: nameInput

                        accessibleName: "Clip name"
                        text: inspector.selectedClip ? inspector.selectedClip.name : ""
                        width: parent.width

                        onEditingFinished: {
                            if (inspector.selectedClip && nameInput.text
                                    !== inspector.selectedClip.name)
                                edit.renameClip(inspector.selectedClip.id, nameInput.text);
                        }
                    }
                }

                // Transform
                Text {
                    color: Theme.textSecondary
                    font.bold: true
                    font.pixelSize: Theme.fs
                    text: "Transform"
                }
                LabelledSlider {
                    id: scale

                    decimals: 3
                    label: "Scale"
                    max: 8
                    min: 0.05
                    modelValue: inspector.selectedClip ? inspector.selectedClip.scale : 1

                    onEdited: v => inspector.applyTransform(v, offsetX.modelValue,
                                                            offsetY.modelValue, rotation.modelValue,
                                                            stretchX.modelValue,
                                                            stretchY.modelValue)
                }
                LabelledSlider {
                    id: offsetX

                    decimals: 3
                    label: "Offset X"
                    max: 3
                    min: -3
                    modelValue: inspector.selectedClip ? inspector.selectedClip.offset_x : 0

                    onEdited: v => inspector.applyTransform(scale.modelValue, v, offsetY.modelValue,
                                                            rotation.modelValue, stretchX.modelValue,
                                                            stretchY.modelValue)
                }
                LabelledSlider {
                    id: offsetY

                    decimals: 3
                    label: "Offset Y"
                    max: 3
                    min: -3
                    modelValue: inspector.selectedClip ? inspector.selectedClip.offset_y : 0

                    onEdited: v => inspector.applyTransform(scale.modelValue, offsetX.modelValue, v,
                                                            rotation.modelValue, stretchX.modelValue,
                                                            stretchY.modelValue)
                }
                LabelledSlider {
                    id: rotation

                    decimals: 1
                    label: "Rotate"
                    max: 180
                    min: -180
                    modelValue: inspector.selectedClip ? inspector.selectedClip.rotation : 0

                    onEdited: v => inspector.applyTransform(scale.modelValue, offsetX.modelValue,
                                                            offsetY.modelValue, v,
                                                            stretchX.modelValue,
                                                            stretchY.modelValue)
                }
                LabelledSlider {
                    id: stretchX

                    decimals: 3
                    label: "Stretch X"
                    max: 10
                    min: 0.1
                    modelValue: inspector.selectedClip ? inspector.selectedClip.stretch_x : 1

                    onEdited: v => inspector.applyTransform(scale.modelValue, offsetX.modelValue,
                                                            offsetY.modelValue, rotation.modelValue,
                                                            v, stretchY.modelValue)
                }
                LabelledSlider {
                    id: stretchY

                    decimals: 3
                    label: "Stretch Y"
                    max: 10
                    min: 0.1
                    modelValue: inspector.selectedClip ? inspector.selectedClip.stretch_y : 1

                    onEdited: v => inspector.applyTransform(scale.modelValue, offsetX.modelValue,
                                                            offsetY.modelValue, rotation.modelValue,
                                                            stretchX.modelValue, v)
                }

                // Compositing
                Text {
                    color: Theme.textSecondary
                    font.bold: true
                    font.pixelSize: Theme.fs
                    text: "Compositing"
                }
                LabelledSlider {
                    id: opacity

                    decimals: 2
                    label: "Opacity"
                    max: 1
                    min: 0
                    modelValue: inspector.selectedClip ? inspector.selectedClip.opacity : 1

                    onEdited: v => edit.setClipOpacity(inspector.selectedClip.id, v)
                }
                LabelledSlider {
                    id: speed

                    decimals: 3
                    label: "Speed"
                    max: 16
                    min: 0.0625
                    modelValue: inspector.selectedClip ? inspector.selectedClip.speed : 1

                    onEdited: v => edit.setClipSpeed(inspector.selectedClip.id, v)
                }
                ToggleButton {
                    checked: inspector.selectedClip ? inspector.selectedClip.reverse : false
                    label: "Reverse"

                    onClicked: edit.setClipReverse(inspector.selectedClip.id, !(
                                                       inspector.selectedClip.reverse))
                }
                LabelledSlider {
                    decimals: 2
                    label: "Fade In"
                    max: inspector.fadeMax
                    min: 0
                    modelValue: inspector.selectedClip ? inspector.selectedClip.fade_in : 0

                    onEdited: v => edit.setClipFades(inspector.selectedClip.id, v,
                                                     inspector.selectedClip.fade_out)
                }
                LabelledSlider {
                    decimals: 2
                    label: "Fade Out"
                    max: inspector.fadeMax
                    min: 0
                    modelValue: inspector.selectedClip ? inspector.selectedClip.fade_out : 0

                    onEdited: v => edit.setClipFades(inspector.selectedClip.id,
                                                     inspector.selectedClip.fade_in, v)
                }
                LabelledSlider {
                    id: pan

                    decimals: 2
                    label: "Pan"
                    max: 1
                    min: -1
                    modelValue: inspector.selectedClip ? inspector.selectedClip.pan : 0

                    onEdited: v => edit.setClipPan(inspector.selectedClip.id, v)
                }

                // Readout of what the model holds, for orientation.
                Text {
                    color: Theme.textSecondary
                    font.family: Theme.fontTechnical
                    font.pixelSize: Theme.fs
                    text: inspector.selectedClip ? ("[" + inspector.selectedClip.kind + "]  "
                                                    + timelineModel.fmt(
                                                        inspector.selectedClip.start) + " - "
                                                    + timelineModel.fmt(
                                                        inspector.selectedClip.start
                                                        + inspector.selectedClip.duration)) : ""
                }

                // Applied effects and their parameters, driven by the
                // `effectParams` controller. The section mirrors the selection
                // onto the controller's clipId itself.
                EffectParamsSection {
                    clipId: inspector.selectedClip ? inspector.selectedClip.id : ""
                    width: parent.width
                }

                // The extension slot contributions for this surface, in host
                // order. Each is a Loader that self-sizes to its content
                // (implicitHeight); a load failure collapses the Loader and
                // shows a visible error surface instead.
                Repeater {
                    model: inspector.inspectorSlots

                    Item {
                        id: slotHost

                        height: Math.max(slotLoader.item ? slotLoader.item.implicitHeight : 0,
                                         errorSurface.visible ? errorSurface.height : 0)
                        width: parent.width

                        Loader {
                            id: slotLoader

                            anchors.left: parent.left
                            anchors.right: parent.right
                            asynchronous: true
                            height: item ? item.implicitHeight : 0
                            source: modelData.url

                            onStatusChanged: {
                                if (status === Loader.Error)
                                    console.warn("extension slot failed to load: " + modelData.id
                                                 + " @ " + modelData.point + " ("
                                                 + slotLoader.source + ")");
                            }
                        }
                        Rectangle {
                            id: errorSurface

                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            border.color: Theme.danger
                            color: Theme.panel
                            height: errorText.implicitHeight + 16
                            radius: Theme.radiusSmall
                            visible: slotLoader.status === Loader.Error

                            Text {
                                id: errorText

                                anchors.centerIn: parent
                                color: Theme.danger
                                font.pixelSize: Theme.fsSm
                                horizontalAlignment: Text.AlignHCenter
                                text: "Slot failed to load: " + modelData.id + "\n"
                                      + slotLoader.source
                                width: parent.width - 16
                                wrapMode: Text.Wrap
                            }
                        }
                    }
                }
            }
        }
    }
}
