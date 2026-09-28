// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The panel this Extension contributes. A panel is arbitrary QML the host
// loads into the extension panel area, so it may load outside the app's Theme
// singleton; this example therefore uses only plain QtQuick types and no
// implicit imports from the app's QML directory.

import QtQuick

Rectangle {
    id: root
    implicitWidth: 320
    implicitHeight: 120
    color: "#1e1e1e"
    radius: 6

    Column {
        anchors.centerIn: parent
        spacing: 8

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            color: "#f0f0f0"
            font.pixelSize: 18
            text: qsTr("Hello from example.hello")
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            color: "#9a9a9a"
            font.pixelSize: 13
            text: qsTr("A native Extension panel")
        }
    }
}
