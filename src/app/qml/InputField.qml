// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// A sunken single-line text field with an optional placeholder, extracted
// from the hand-rolled input boxes repeated across MediaBin (search),
// ClipInspector (name) and ExportDialog (preset, output path, number fields).
// Exposes the TextInput as `input` for callers that need a validator or
// fine-grained signals; the common cases bind `text`, `placeholder`,
// `textChanged` and `editingFinished` directly.

import QtQuick

Rectangle {
    id: field

    // The name assistive tech announces. Defaults to the placeholder (the
    // field's only visible label in most call sites); a caller with a separate
    // visible label sets this explicitly.
    property string accessibleName: placeholder
    property int horizontalAlignment: TextInput.AlignLeft
    property alias input: input
    property string placeholder: ""
    property alias text: input.text

    // No `signal textChanged()` here: the `text` alias above already
    // generates it, and declaring it again is an invalid override of a
    // property-change signal - which Qt reports as 'Type TextField
    // unavailable' three levels away, in Main.qml.
    signal editingFinished

    Accessible.description: placeholder
    Accessible.name: accessibleName
    Accessible.role: Accessible.EditableText
    border.color: input.activeFocus ? Theme.accentBorder : Theme.controlBorder
    // A field must read as an editable well against the panel it sits on: the
    // well fill alone is too close to the panel ground, so the outline carries
    // the affordance and brightens to the accent while focused.
    border.width: 1
    color: Theme.well
    height: 26
    radius: Theme.radiusSmall

    TextInput {
        id: input

        anchors.fill: parent
        anchors.leftMargin: 6
        anchors.rightMargin: 6
        clip: true
        color: Theme.textPrimary
        font.pixelSize: Theme.fsMd
        horizontalAlignment: field.horizontalAlignment
        selectByMouse: true
        verticalAlignment: TextInput.AlignVCenter

        onEditingFinished: field.editingFinished()
        onTextChanged: field.textChanged()
    }
    Text {
        anchors.left: parent.left
        anchors.leftMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        color: Theme.textDisabled
        font.pixelSize: Theme.fsMd
        text: field.placeholder
        visible: field.text === "" && field.placeholder !== ""
    }
}
