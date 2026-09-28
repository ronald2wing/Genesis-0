// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The scopes pane: the three scopes the ScopesController publishes - the
// Rec.709 luma histogram (256 bins), the per-channel waveform (min/max per
// column), and the Cb/Cr vectorscope (a 256x256 density plane) - plus an
// enabled toggle and a mode selector. One Canvas draws whichever mode is
// selected, so only the active scope's arrays are converted from C++ per
// repaint. While `scopes.idle` is true (no frame has arrived, or the scopes
// are switched off) the pane shows an idle note instead of a blank plot or a
// stall.
//
// Pure model/view (R4): every read comes from the controller's properties and
// every write goes through its setters; no media work happens here, on or off
// the UI thread.

import QtQuick

Panel {
    id: pane

    // The three modes, mirroring ScopesController::Mode (Histogram=0,
    // Waveform=1, Vectorscope=2).
    property var modes: [
        {
            name: "Histogram",
            value: 0
        },
        {
            name: "Waveform",
            value: 1
        },
        {
            name: "Vectorscope",
            value: 2
        }
    ]

    // The controller republishes every scope on frameChanged; repaint then, and
    // when the mode or the idle gate changes, so the plot swaps to the selected
    // scope (or back to the idle note) without a stale frame.
    Connections {
        function onFrameChanged() {
            plot.requestPaint();
        }
        function onIdleChanged() {
            plot.requestPaint();
        }
        function onModeChanged() {
            plot.requestPaint();
        }

        target: scopes
    }
    Item {
        anchors.fill: parent
        anchors.margins: 12

        Text {
            id: title

            anchors.top: parent.top
            color: Theme.textPrimary
            font.bold: true
            font.pixelSize: Theme.fsXl
            text: "Scopes"
        }
        Rectangle {
            id: divider

            anchors.top: title.bottom
            anchors.topMargin: 8
            color: Theme.panelBorder
            height: 1
            width: parent.width
        }

        // Controls: the enabled toggle, then the mode chips.
        Row {
            id: controls

            anchors.top: divider.bottom
            anchors.topMargin: 8
            spacing: 6
            width: parent.width

            ToggleButton {
                checked: scopes.enabled
                label: "Enabled"

                onClicked: scopes.enabled = !scopes.enabled
            }
            Repeater {
                model: pane.modes

                ToggleButton {
                    checked: scopes.mode === modelData.value
                    label: modelData.name

                    onClicked: scopes.mode = modelData.value
                }
            }
        }

        // The plot area: the selected scope, or the idle note over the well.
        Item {
            id: plotArea

            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: controls.bottom
            anchors.topMargin: 8

            Rectangle {
                anchors.fill: parent
                color: Theme.well
                radius: Theme.radiusSmall
            }
            Canvas {
                id: plot

                // The Rec.709 luma histogram: a bar per bin, scaled to the
                // tallest bin so a quiet frame never collapses to a flat line.
                function drawHistogram(ctx, w, h) {
                    const bins = scopes.histogram;
                    const n = bins ? bins.length : 0;
                    if (n === 0)
                        return;
                    let max = 1;
                    for (let i = 0; i < n; i++)
                        max = Math.max(max, bins[i]);
                    const barW = Math.max(w / n, 1);
                    ctx.fillStyle = Theme.accent;
                    for (let i = 0; i < n; i++) {
                        const bh = bins[i] / max * h;
                        if (bh > 0)
                            ctx.fillRect(i * barW, h - bh, barW, bh);
                    }
                }

                // The vectorscope: the Cb/Cr density plane, drawn at the plot's
                // own resolution so a 256x256 grid scales with the pane. Each
                // pixel's alpha tracks its bin's density.
                function drawVectorscope(ctx, w, h) {
                    const dens = scopes.vectorscope;
                    const size = 256;
                    if (!dens || dens.length !== size * size)
                        return;
                    let max = 1;
                    for (let i = 0; i < dens.length; i++)
                        max = Math.max(max, dens[i]);
                    const r = Math.round(Theme.accent.r * 255);
                    const g = Math.round(Theme.accent.g * 255);
                    const b = Math.round(Theme.accent.b * 255);
                    const img = ctx.createImageData(w, h);
                    const px = img.data;
                    for (let y = 0; y < h; y++) {
                        const row = Math.floor(y / h * size);
                        for (let x = 0; x < w; x++) {
                            const col = Math.floor(x / w * size);
                            const d = dens[row * size + col];
                            const a = d > 0 ? Math.round(d / max * 255) : 0;
                            const idx = (y * w + x) * 4;
                            px[idx] = r;
                            px[idx + 1] = g;
                            px[idx + 2] = b;
                            px[idx + 3] = a;
                        }
                    }
                    ctx.putImageData(img, 0, 0);
                }

                // The waveform: per-column vertical span (min..max). The RGB
                // channels sit under the luma trace. The theme has no dedicated
                // scope-channel colours, so the status/kind hues stand in for
                // red/green/blue.
                function drawWaveform(ctx, w, h) {
                    const lumaMin = scopes.waveformLumaMin;
                    const n = lumaMin ? lumaMin.length : 0;
                    if (n === 0)
                        return;
                    const colW = w / n;
                    const drawChannel = (minArr, maxArr, color, alpha) => {
                        ctx.globalAlpha = alpha;
                        ctx.fillStyle = color;
                        for (let i = 0; i < n; i++) {
                            // Untouched columns keep the empty sentinel
                            // (min 255, max 0): skip them.
                            if (maxArr[i] < minArr[i])
                                continue;
                            const top = h - maxArr[i] / 255 * h;
                            const bottom = h - minArr[i] / 255 * h;
                            ctx.fillRect(i * colW, top, Math.max(colW, 1), Math.max(bottom - top,
                                                                                    1));

                        }
                    };
                    drawChannel(scopes.waveformRedMin, scopes.waveformRedMax, Theme.danger, 0.40);
                    drawChannel(scopes.waveformGreenMin, scopes.waveformGreenMax, Theme.success,
                                0.40);
                    drawChannel(scopes.waveformBlueMin, scopes.waveformBlueMax, Theme.kindVideo,
                                0.40);
                    drawChannel(lumaMin, scopes.waveformLumaMax, Theme.accent, 0.85);
                    ctx.globalAlpha = 1.0;
                }

                anchors.fill: parent
                anchors.margins: 4
                visible: !scopes.idle

                onPaint: {
                    const ctx = getContext("2d");
                    const w = width, h = height;
                    ctx.clearRect(0, 0, w, h);
                    if (scopes.mode === 0)
                        drawHistogram(ctx, w, h);
                    else if (scopes.mode === 1)
                        drawWaveform(ctx, w, h);
                    else
                        drawVectorscope(ctx, w, h);
                }
            }

            // The idle note: shown before a frame arrives and while the
            // scopes are off. Never a blank plot.
            Text {
                anchors.centerIn: parent
                color: Theme.textDisabled
                font.pixelSize: Theme.fsMd
                text: scopes.enabled ? "Scopes idle — waiting for a frame." :
                                       "Scopes off — enable to sample."
                visible: scopes.idle
            }
        }
    }
}
