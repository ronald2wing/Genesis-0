// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// A labelled button with rest/hover/pressed/disabled states. Replaces the
// hand-rolled ActionButton (TransportBar), the export/close/cancel buttons
// (ExportDialog), KeyAction (KeyframesPane) and the header Export button
// (Main.qml), so one component draws every push button. `accent` marks the
// primary action in a group. Callers own the state and decide what a click
// means; the component only reports it.

import QtQuick

Rectangle {
    id: btn

    property bool accent: false
    property url iconDisabledSource: ""
    property bool iconOnly: false
    // An optional leading glyph. The colour is baked into the SVG (see
    // assets/icons/README.md) because runtime tinting does not render under
    // software GL; `iconDisabledSource` is the dimmed variant shown when the
    // button is disabled. `iconOnly` drops the visible label and squares the
    // button; the label still names the control for assistive tech, so an
    // icon-only caller must keep it.
    property url iconSource: ""
    property string label: ""

    signal clicked

    // A push button is a button to assistive tech; the visible label is its
    // name unless a caller overrides it (an icon-only or abbreviated control).
    Accessible.name: label
    Accessible.role: Accessible.Button
    border.color: accent ? Theme.accentBorder : Theme.controlBorder
    color: !enabled ? Theme.controlDisabled : accent ? (press.pressed ? Theme.accentHover :
                                                                        Theme.accent) : (
                                                           press.pressed ? Theme.controlPressed : (
                                                                               press.containsMouse
                                                                               ? Theme.controlHover :
                                                                                 Theme.control))
    height: Theme.controlHeight
    radius: Theme.radius
    width: iconOnly ? Theme.controlHeight : (iconSource !== "" ? labelText.implicitWidth + 24 + 24 :
                                                                 labelText.implicitWidth + 24)

    Accessible.onPressAction: if (enabled)
                                  btn.clicked()

    Row {
        anchors.centerIn: parent
        spacing: 6

        Image {
            anchors.verticalCenter: parent.verticalCenter
            fillMode: Image.PreserveAspectFit
            source: !btn.enabled && btn.iconDisabledSource !== "" ? btn.iconDisabledSource :
                                                                    btn.iconSource

            sourceSize.height: 18
            sourceSize.width: 18
            visible: btn.iconSource !== ""
        }
        Text {
            id: labelText

            anchors.verticalCenter: parent.verticalCenter
            color: !enabled ? Theme.textDisabled : (accent ? Theme.onAccent : Theme.textPrimary)
            font.pixelSize: Theme.fsLg
            text: btn.label
            visible: !btn.iconOnly
        }
    }
    MouseArea {
        id: press

        anchors.fill: parent
        enabled: btn.enabled
        hoverEnabled: true

        onClicked: btn.clicked()
    }
}
