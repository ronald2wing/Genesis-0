// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// A labelled toggle button: a pill whose filled state shows `checked`. Shared
// by the timeline's tool toggles (snap/ripple) and the track headers'
// mute/visible switches, so one component draws every stateful button in the
// strip. Pure presentation - the caller owns the state and decides what a
// click means (an edit command, or a tool flip).

import QtQuick

Rectangle {
    id: btn

    property bool checked: false
    property bool enabled: true
    property string label: ""

    signal clicked

    border.color: checked ? Theme.accentBorder : Theme.controlBorder
    color: !enabled ? Theme.controlDisabled : (checked ? Theme.accent : Theme.control)
    height: 22
    radius: Theme.radiusSmall
    width: labelText.implicitWidth + 16

    Text {
        id: labelText

        anchors.centerIn: parent
        color: !enabled ? Theme.textDisabled : (checked ? Theme.onAccent : Theme.textSecondary)
        font.pixelSize: Theme.fsMd
        text: btn.label
    }
    MouseArea {
        anchors.fill: parent
        enabled: btn.enabled

        onClicked: btn.clicked()
    }
}
