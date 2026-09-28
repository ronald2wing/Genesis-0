// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The GPU status indicator: a small, always-visible pill in the transport
// strip that reads the `gpu` controller (HardwareStatus) and shows whether the
// GPU is doing the work. A short label - "GPU: NVIDIA" or "GPU: software
// (llvmpipe)" - and, on hover, a detail card listing the renderer, decode,
// encode and AI modes. Pure presentation: the controller owns the values.

import QtQuick

Item {
    id: indicator

    readonly property string label: {
        if (gpu.softwareGl)
            return shortName !== "" ? "GPU: software (" + shortName + ")" : "GPU: software";
        return shortName !== "" ? "GPU: " + shortName : "GPU: unavailable";
    }

    // The short vendor/rasteriser name: the renderer string up to the first
    // space or "(", so "NVIDIA GeForce ..." becomes "NVIDIA" and
    // "llvmpipe (LLVM ...)" becomes "llvmpipe". The Mesa GL driver prefixes
    // the vendor on Intel/AMD ("Mesa Intel(R) ...", "Mesa DRI ..."), so that
    // driver name is stripped to let the vendor read first.
    readonly property string shortName: {
        let r = gpu.renderer;
        if (r === "")
            return "";
        if (r.indexOf("Mesa DRI ") === 0)
            r = r.substring(9);
        else if (r.indexOf("Mesa ") === 0)
            r = r.substring(5);
        let end = r.length;
        const space = r.indexOf(" ");
        if (space >= 0)
            end = Math.min(end, space);
        const open = r.indexOf("(");
        if (open >= 0)
            end = Math.min(end, open);
        return r.substring(0, end);
    }

    // A status readout, not a control: the short label is the name and the
    // hover card's detail is the description, so the full renderer/mode story
    // is available without a pointer.
    Accessible.description: "Renderer: " + (gpu.renderer !== "" ? gpu.renderer : "unavailable")
                            + "; decode: " + gpu.decode + "; encode: " + gpu.encode + "; AI: "
                            + gpu.ai + (gpu.nvidiaAvailable ? "; NVIDIA: " + gpu.nvidiaElements.join(
                                                                  ", ") : "") + (gpu.nvidiaHint
                                                                                 !== "" ? "; "
                                                                                          + gpu.nvidiaHint :
                                                                                          "")
    Accessible.name: label
    Accessible.role: Accessible.StaticText
    height: 22
    width: pill.width

    Rectangle {
        id: pill

        border.color: gpu.softwareGl ? Theme.wellBorder : Theme.panelBorder
        color: gpu.softwareGl ? Theme.well : Theme.panel
        height: 22
        radius: Theme.radiusSmall
        width: labelText.implicitWidth + 16

        Text {
            id: labelText

            anchors.centerIn: parent
            color: Theme.textSecondary
            font.family: Theme.fontTechnical
            font.pixelSize: Theme.fsSm
            text: indicator.label
        }
    }
    MouseArea {
        id: pillHover

        anchors.fill: pill
        hoverEnabled: true
    }

    // The detail card, shown on hover: the full renderer string and the three
    // modes. Drops below the pill so it stays inside the transport strip (the
    // wrapper clips anything that would overflow upward).
    Panel {
        id: detail

        anchors.right: pill.right
        anchors.top: pill.bottom
        anchors.topMargin: 6
        height: detailColumn.height + 16
        radius: Theme.radius
        visible: pillHover.containsMouse
        width: detailColumn.width + 24
        z: 95

        Column {
            id: detailColumn

            anchors.centerIn: parent
            spacing: 4

            Text {
                color: Theme.textPrimary
                font.family: Theme.fontTechnical
                font.pixelSize: Theme.fsSm
                text: "Renderer: " + (gpu.renderer !== "" ? gpu.renderer : "unavailable")
            }
            Text {
                color: Theme.textSecondary
                font.pixelSize: Theme.fsSm
                text: "Decode: " + gpu.decode + " (" + gpu.decodePolicy + ")"
            }
            Text {
                color: Theme.textSecondary
                font.pixelSize: Theme.fsSm
                text: "Encode: " + gpu.encode + (gpu.encodeElement !== "" ? " ("
                                                                            + gpu.encodeElement
                                                                            + ")" : "")
            }
            Text {
                color: Theme.textSecondary
                font.pixelSize: Theme.fsSm
                text: gpu.nvidiaElements.length > 0 ? "NVIDIA: available ("
                                                      + gpu.nvidiaElements.join(", ") + ")" :
                                                      "NVIDIA: available"
                visible: gpu.nvidiaAvailable
                width: 300
                wrapMode: Text.WordWrap
            }
            Text {
                color: Theme.textSecondary
                font.pixelSize: Theme.fsSm
                text: "AI: " + gpu.ai
            }
            Text {
                color: Theme.amberWash
                font.pixelSize: Theme.fsSm
                text: gpu.nvidiaHint
                visible: gpu.nvidiaHint !== ""
                width: 300
                wrapMode: Text.WordWrap
            }
        }
    }
}
