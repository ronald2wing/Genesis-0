// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The title gallery as an inspector panel (the builtin `genesis.titles`
// extension). It is the QML port of the old TitleGalleryDialog: the template
// list, the per-slot text fields, the duration field and the Add button, but
// laid out as a pane rather than a dimmed overlay.
//
// The panel owns no host state. It reaches the host through the `extensions`
// context property's `call(method, paramsJson)` bridge, which returns the same
// JSON-RPC reply envelope the CLI sees:
//
//   * `template.list`  -> {"result": [{name, slots:[{placeholder, style}]}]}
//   * `template.fill`  -> {"result": [{text, style}]}
//   * `edit.apply`     -> {"result": <editor view>} or {"error": {code, message}}
//
// Add mirrors the old controller's semantics exactly: one `addTextClip` per
// filled slot, `above = true` so each successive slot lands on the lane above
// the last, the playhead (`controller.position()`) as the start, the entered
// duration when positive (the editorial default otherwise), and the whole set
// wrapped in one `batch` so a single undo removes every slot's clip. The
// `style` from `template.fill` is passed through verbatim - it is already the
// exact shape `addTextClip` decodes.
//
// The panel is loaded from the installed-extension store, outside the app's
// QML directory, so it imports the app module (`GenesisApp`) for the design
// system (Theme, Panel, Button, InputField) and reads the shared context
// properties (`extensions`, `controller`, `timelineModel`). A template library
// that is unavailable or empty degrades to a visible message, never a blank
// pane.

import QtQuick
import GenesisApp 1.0

Item {
    id: panel

    // Add is live only with a valid pick and exactly one text per slot.
    readonly property bool canAdd: selectedTemplate !== null && slotTexts.length === slotCount

    // The duration field, seconds. Zero/blank means the host's default.
    property double duration: 0.0

    // The last error the host returned, shown in the danger colour. Empty when
    // the last call succeeded.
    property string errorText: ""

    // The selected template index, or -1 for none.
    property int selectedIndex: -1

    // The selected template map, or null.
    readonly property var selectedTemplate: (selectedIndex >= 0 && selectedIndex
                                             < templates.length) ? templates[selectedIndex] : null
    readonly property int slotCount: selectedTemplate ? selectedTemplate.slots.length : 0

    // The per-slot texts for the selected template, one entry per slot.
    property var slotTexts: []

    // The discovered templates, one map per template:
    // { name: string, slots: [{placeholder, style}] }. Empty until the first
    // `template.list` reply lands, and empty again if that call errors.
    property var templates: []

    // Fill the pick and apply one batch of addTextClip commands. Returns true
    // when the edit applied.
    function addTitle() {
        if (!canAdd)
            return false;
        panel.errorText = "";

        var fillParams = JSON.stringify({
                                            template: selectedTemplate.name,
                                            texts: slotTexts
                                        });
        var filled = reply.resultOf(extensions.call("template.fill", fillParams));
        if (filled === null)
            return false;

        var commands = [];
        for (var i = 0; i < filled.length; i++) {
            var command = {
                op: "addTextClip",
                above: true,
                start: controller.position(),
                style: filled[i].style
            };
            if (duration > 0)
                command.duration = duration;
            commands.push(command);
        }

        var applyParams = JSON.stringify({
                                             command: {
                                                 op: "batch",
                                                 commands: commands
                                             }
                                         });
        if (reply.resultOf(extensions.call("edit.apply", applyParams)) === null)
            return false;
        return true;
    }

    // Discover the library. Called once on completion; the library is static on
    // disk, so there is no per-edit refresh.
    function loadTemplates() {
        panel.errorText = "";
        var result = reply.resultOf(extensions.call("template.list", "{}"));
        if (result === null) {
            panel.templates = [];
            panel.selectedIndex = -1;
            return;
        }
        panel.templates = result;
        panel.selectedIndex = result.length > 0 ? 0 : -1;
    }

    // Re-seed the slot fields for the current pick.
    function reseedSlots() {
        var texts = [];
        for (var i = 0; i < panel.slotCount; i++)
            texts.push("");
        panel.slotTexts = texts;
    }

    Component.onCompleted: loadTemplates()
    onSelectedIndexChanged: reseedSlots()

    AiReply {
        id: reply

        errorTarget: panel
    }
    Column {
        anchors.fill: parent
        anchors.margins: Theme.pad
        spacing: Theme.spacing

        Text {
            color: Theme.textPrimary
            font.bold: true
            font.pixelSize: Theme.fsTitle
            text: "Titles"
        }
        Rectangle {
            color: Theme.panelBorder
            height: 1
            width: parent.width
        }

        // The host's refusal, shown only when a call failed.
        Text {
            color: Theme.danger
            font.pixelSize: Theme.fs
            text: panel.errorText
            visible: panel.errorText !== ""
            width: parent.width
            wrapMode: Text.Wrap
        }

        // Empty state: the library is unavailable or has no templates.
        Text {
            color: Theme.textDisabled
            font.pixelSize: Theme.fsMd
            text: "No title templates available."
            visible: panel.templates.length === 0
            width: parent.width
            wrapMode: Text.Wrap
        }

        // ── template list ────────────────────────────────────────────────
        Column {
            spacing: Theme.gap
            visible: panel.templates.length > 0
            width: parent.width

            Text {
                color: Theme.textSecondary
                font.pixelSize: Theme.fs
                text: "Templates (" + panel.templates.length + ")"
            }
            ListView {
                id: templateList

                clip: true
                height: Math.min(contentHeight, 180)
                model: panel.templates
                spacing: 4
                width: parent.width

                delegate: Rectangle {
                    Accessible.checkable: true
                    Accessible.checked: panel.selectedIndex === index
                    Accessible.name: modelData.name + ", " + modelData.slots.length + " slot" + (
                                         modelData.slots.length === 1 ? "" : "s")
                    Accessible.role: Accessible.ListItem
                    color: panel.selectedIndex === index ? Theme.accentSoft : (index % 2 === 0
                                                                               ? Theme.rowAlt :
                                                                                 "transparent")
                    height: 40
                    radius: Theme.radiusSmall
                    width: templateList.width

                    Accessible.onPressAction: panel.selectedIndex = index

                    MouseArea {
                        anchors.fill: parent

                        onClicked: panel.selectedIndex = index
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
                            color: panel.selectedIndex === index ? Theme.textPrimary :
                                                                   Theme.textSecondary
                            font.family: Theme.fontTechnical
                            font.pixelSize: Theme.fs
                            text: modelData.slots.length + " slot" + (modelData.slots.length === 1 ? "" :
                                                                                                     "s")
                        }
                    }
                }
            }
        }

        // ── the pick's fields ────────────────────────────────────────────
        Column {
            spacing: Theme.gap
            visible: panel.selectedTemplate !== null
            width: parent.width

            Text {
                color: Theme.textSecondary
                font.pixelSize: Theme.fs
                text: "Text"
            }
            Repeater {
                model: panel.slotCount

                delegate: InputField {
                    accessibleName: panel.selectedTemplate
                                    && panel.selectedTemplate.slots[index].placeholder !== ""
                                    ? panel.selectedTemplate.slots[index].placeholder : "Slot " + (
                                          index + 1) + " text"
                    placeholder: panel.selectedTemplate
                                 ? panel.selectedTemplate.slots[index].placeholder : ""
                    text: panel.slotTexts[index] !== undefined ? panel.slotTexts[index] : ""
                    width: parent.width

                    onTextChanged: {
                        var texts = panel.slotTexts.slice();
                        texts[index] = text;
                        panel.slotTexts = texts;
                    }
                }
            }
            Text {
                color: Theme.textSecondary
                font.pixelSize: Theme.fs
                text: "Placement"
            }
            Row {
                spacing: Theme.spacing

                Column {
                    spacing: 2

                    Text {
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fsSm
                        text: "Start (playhead)"
                    }
                    InputField {
                        accessibleName: "Start (playhead)"
                        input.color: Theme.textDisabled
                        input.readOnly: true
                        text: timelineModel.fmt(controller.position())
                        width: 110
                    }
                }
                Column {
                    spacing: 2

                    Text {
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fsSm
                        text: "Duration (s)"
                    }
                    InputField {
                        accessibleName: "Duration (s)"
                        placeholder: "default"
                        text: panel.duration > 0 ? String(panel.duration) : ""
                        width: 90

                        input.validator: DoubleValidator {
                            bottom: 0
                        }

                        onEditingFinished: {
                            var v = parseFloat(text);
                            panel.duration = isNaN(v) ? 0 : v;
                        }
                    }
                }
            }
        }

        // ── the action ───────────────────────────────────────────────────
        Button {
            accent: true
            enabled: panel.canAdd
            label: "Add"

            onClicked: panel.addTitle()
        }
    }
}
