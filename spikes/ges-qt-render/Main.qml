// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Phase 0 spike scene: a Qt Quick window containing the GES-fed video item,
// with an on-screen readout of the frame path actually observed.

import QtQuick
import QtQuick.Window
import GesSpike 1.0

Window {
    id: root
    width: 960
    height: 600
    visible: true
    title: "GES -> Qt Quick frame-path spike"
    color: "#101014"

    Column {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Text {
            text: "GES -> Qt Quick frame-path spike"
            color: "#e8e8ee"
            font.pixelSize: 20
            font.bold: true
        }

        Rectangle {
            width: parent.width
            height: 1
            color: "#2a2a34"
        }

        Text {
            text: "source: " + video.source
            color: "#9a9aa8"
            font.pixelSize: 13
        }

        Text {
            text: "frames observed: " + video.frameCount
            color: "#e8e8ee"
            font.pixelSize: 15
        }

        Text {
            text: "frames rendered: " + video.rendered
            color: video.rendered > 0 ? "#5ad07a" : "#d07a5a"
            font.pixelSize: 15
            font.bold: true
        }

        Text {
            text: "GPU-resident: " + (video.gpuResident ? "YES" : "no")
            color: video.gpuResident ? "#5ad07a" : "#d07a5a"
            font.pixelSize: 15
            font.bold: true
        }

        Text {
            text: "frame path: " + video.framePath
            color: "#c8c8d4"
            font.pixelSize: 13
            wrapMode: Text.Wrap
            width: parent.width
        }

        // The item itself. It paints: each GL frame is committed to the scene
        // graph as a textured node. The "frames rendered" readout above counts
        // committed nodes, so a climbing value means the texture is presented.
        GesVideoItem {
            id: video
            width: parent.width
            height: 200
            source: spikeSource
        }
    }
}
