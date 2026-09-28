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
    property bool enabled: true
    property string label: ""

    signal clicked

    border.color: accent ? Theme.accentBorder : Theme.controlBorder
    color: !enabled ? Theme.controlDisabled : accent ? (press.pressed ? Theme.accentHover :
                                                                        Theme.accent) : (
                                                           press.pressed ? Theme.controlPressed : (
                                                                               hover.containsMouse
                                                                               ? Theme.controlHover :
                                                                                 Theme.control))
    height: Theme.controlHeight
    radius: Theme.radius
    width: labelText.implicitWidth + 24

    Text {
        id: labelText

        anchors.centerIn: parent
        color: !enabled ? Theme.textDisabled : (accent ? Theme.onAccent : Theme.textPrimary)
        font.pixelSize: Theme.fsLg
        text: btn.label
    }
    MouseArea {
        id: press

        anchors.fill: parent
        enabled: btn.enabled
        hoverEnabled: true

        onClicked: btn.clicked()
    }
}
