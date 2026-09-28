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
    property url iconCheckedSource: ""
    property bool iconOnly: false
    // An optional leading glyph. The colour is baked into the SVG (see
    // assets/icons/README.md) because runtime tinting does not render under
    // software GL; `iconCheckedSource` is the on-accent variant shown when the
    // toggle is checked. `iconOnly` drops the visible label and squares the
    // button; the label still names the control for assistive tech, so an
    // icon-only caller must keep it.
    property url iconSource: ""
    property string label: ""

    signal clicked

    // A stateful button: expose it as a check box so the checked state is
    // announced, with the visible label as its name. A caller may override the
    // name for an abbreviated chip (e.g. "Vis" -> "Visible").
    Accessible.checkable: true
    Accessible.checked: checked
    Accessible.name: label
    Accessible.role: Accessible.CheckBox
    border.color: checked ? Theme.accentBorder : Theme.controlBorder
    color: !enabled ? Theme.controlDisabled : (checked ? Theme.accent : Theme.control)
    height: 22
    radius: Theme.radiusSmall
    width: iconOnly ? 22 : (iconSource !== "" ? labelText.implicitWidth + 16 + 20 :
                                                labelText.implicitWidth + 16)

    Accessible.onPressAction: if (enabled)
                                  btn.clicked()

    Row {
        anchors.centerIn: parent
        spacing: 4

        Image {
            anchors.verticalCenter: parent.verticalCenter
            fillMode: Image.PreserveAspectFit
            source: btn.checked && btn.iconCheckedSource !== "" ? btn.iconCheckedSource :
                                                                  btn.iconSource

            sourceSize.height: 14
            sourceSize.width: 14
            visible: btn.iconSource !== ""
        }
        Text {
            id: labelText

            anchors.verticalCenter: parent.verticalCenter
            color: !enabled ? Theme.textDisabled : (checked ? Theme.onAccent : Theme.textSecondary)
            font.pixelSize: Theme.fsMd
            text: btn.label
            visible: !btn.iconOnly
        }
    }
    MouseArea {
        anchors.fill: parent
        enabled: btn.enabled

        onClicked: btn.clicked()
    }
}
