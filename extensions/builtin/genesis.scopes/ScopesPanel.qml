// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The scopes pane as an inspector panel (the builtin `genesis.scopes`
// extension). It is the QML port of the old ScopesPane: the Rec.709 luma
// histogram (256 bins), the per-channel waveform (min/max per column), the
// Cb/Cr vectorscope (a 256x256 density plane) and the RGB parade (the three
// color channels as three waveform columns on one stepped IRE scale), with the
// same mode chips and one Canvas drawing whichever mode is selected.
//
// The panel owns no host state. It reaches the host through the `extensions`
// context property's `call(method, paramsJson)` bridge, which returns the same
// JSON-RPC reply envelope the CLI sees:
//
//   * `scopes.snapshot` -> {"result": {idle, mode, kind, ...}} where the
//     payload depends on the requested `kind`:
//       - "histogram"   -> {levels: [256 ints]}
//       - "waveform"    -> {lumaMin/Max, redMin/Max, greenMin/Max,
//                           blueMin/Max: [256 ints each]}
//       - "vectorscope" -> {size: 256, density: [65536 ints]}
//       - "parade"      -> {bands: [{channel, min, max} x 3], one band per
//                           R|G|B channel}
//
// There is no event stream in the SDK yet, so the panel polls the snapshot on
// a ~10 Hz Timer while it is visible (the old pane repainted on the
// controller's frameChanged signal; the bridge has no push channel). The
// controller's own sampling is gated by `scopes.enabled`, which has no bridge
// verb, so the panel drives it through the shared `scopes` context property:
// enabling while visible and disabling when hidden, so the off-thread sampler
// runs only while the pane is on screen.
//
// The panel is loaded from the installed-extension store, outside the app's
// QML directory, so it imports the app module (`GenesisApp`) for the design
// system (Theme, ToggleButton) and reads the shared context properties
// (`extensions`, `scopes`). A snapshot that fails, or a frame that has not
// arrived, degrades to a visible note, never a blank plot.

import QtQuick
import GenesisApp 1.0

Item {
    id: panel

    // The last error the host returned, shown in the danger colour. Empty when
    // the last call succeeded.
    property string errorText: ""

    // Whether the latest frame's bins read on a perceptual (HDR) nit scale,
    // from the snapshot's `hdr` field.
    property bool hdr: false

    // True before any frame has arrived and while the scopes are off, read
    // from the snapshot's `idle` field.
    property bool idle: true

    // Which scope the panel draws: Histogram (0), Waveform (1), Vectorscope
    // (2), Parade (3), mirroring ScopesController::Mode. The panel passes the
    // matching `kind` to the bridge; the controller's own mode is not
    // consulted.
    property int mode: 0

    // The four modes, in chip order.
    readonly property var modes: [
        {
            name: "Histogram",
            value: 0,
            kind: "histogram"
        },
        {
            name: "Waveform",
            value: 1,
            kind: "waveform"
        },
        {
            name: "Vectorscope",
            value: 2,
            kind: "vectorscope"
        },
        {
            name: "Parade",
            value: 3,
            kind: "parade"
        }
    ]

    // The scope scale maximum in nits (1000 HLG, 10000 PQ), from the
    // snapshot's `scaleMax` field; 255 (the SDR code-value ceiling) otherwise.
    property int scaleMax: 255

    // The last parsed snapshot result, or null before the first reply. Its
    // shape depends on the requested kind (see the header).
    property var snapshot: null

    // The bridge `kind` for the selected mode.
    function kindName() {
        return panel.modes[panel.mode].kind;
    }

    // Fetch one snapshot and repaint. A malformed or refused reply sets
    // `errorText` and leaves the last good snapshot in place, so a transient
    // failure does not blank the plot.
    function poll() {
        var reply = extensions.call("scopes.snapshot", JSON.stringify({
                                                                          kind: kindName()
                                                                      }));
        var parsed;
        try {
            parsed = JSON.parse(reply);
        } catch (e) {
            panel.errorText = "Malformed host reply";
            return;
        }
        if (parsed.error !== undefined) {
            panel.errorText = parsed.error.message !== undefined ? parsed.error.message :
                                                                   "Host refused the request";
            return;
        }
        if (parsed.result === undefined) {
            panel.errorText = "Host reply carried no result";
            return;
        }
        panel.errorText = "";
        panel.snapshot = parsed.result;
        panel.idle = parsed.result.idle === true;
        panel.hdr = parsed.result.hdr === true;
        panel.scaleMax = parsed.result.scaleMax !== undefined ? parsed.result.scaleMax : 255;
        plot.requestPaint();
    }

    Component.onCompleted: {
        if (visible)
            scopes.enabled = true;
    }

    // The off-thread sampler runs only while the pane is on screen. The
    // controller's `enabled` has no bridge verb, so it is driven through the
    // shared context property; the panel's own Timer is the poll gate.
    onVisibleChanged: {
        scopes.enabled = visible;
        if (visible)
            poll();
    }

    // ~10 Hz: the old pane repainted on frameChanged (~30 Hz); the bridge is a
    // synchronous call, so a slower poll keeps the UI thread free while still
    // reading as live.
    Timer {
        interval: 100
        repeat: true
        running: panel.visible

        onTriggered: panel.poll()
    }
    Column {
        anchors.fill: parent
        anchors.margins: Theme.pad
        spacing: Theme.spacing

        Text {
            color: Theme.textPrimary
            font.bold: true
            font.pixelSize: Theme.fsTitle
            text: "Scopes"
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

        // Mode chips: the existing toggle convention. Four chips no longer fit
        // a narrow inspector on one line, so a Flow wraps the tail instead of
        // clipping it past the pane's right edge.
        Flow {
            id: controls

            spacing: 6
            width: parent.width

            Repeater {
                model: panel.modes

                ToggleButton {
                    checked: panel.mode === modelData.value
                    label: modelData.name

                    onClicked: {
                        panel.mode = modelData.value;
                        panel.poll();
                    }
                }
            }
        }

        // The HDR tag and nit scale label, shown only when the latest frame was
        // reduced on a perceptual nit scale. scaleMax is the scope scale
        // maximum (1000 nits for HLG, 10000 for PQ), so the label reads
        // "0–<scaleMax> nits".
        Row {
            spacing: Theme.gap
            visible: panel.hdr

            Rectangle {
                color: Theme.accentSoft
                height: 18
                radius: Theme.radiusSmall
                width: hdrTag.implicitWidth + 12

                Text {
                    id: hdrTag

                    anchors.centerIn: parent
                    color: Theme.accent
                    font.bold: true
                    font.family: Theme.fontTechnical
                    font.pixelSize: Theme.fsSm
                    text: "HDR"
                }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.textSecondary
                font.family: Theme.fontTechnical
                font.pixelSize: Theme.fsSm
                text: "0–" + panel.scaleMax + " nits"
            }
        }

        // The plot area: the selected scope, or the idle note over the well.
        Item {
            id: plotArea

            height: parent.height - y
            width: parent.width

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
                    const bins = panel.snapshot ? panel.snapshot.levels : null;
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

                // The parade: the three color channels drawn as three columns
                // (R|G|B) on one stepped IRE/voltage scale. Each band carries
                // its channel's per-column min/max span; the same status/kind
                // hues as the waveform stand in for red/green/blue.
                function drawParade(ctx, w, h) {
                    const bands = panel.snapshot ? panel.snapshot.bands : null;
                    if (!bands || bands.length === 0)
                        return;
                    // The stepped IRE graticule: faint reference lines at the
                    // quarter-voltage steps, shared by every column.
                    ctx.strokeStyle = Theme.wellBorder;
                    ctx.lineWidth = 1;
                    for (let s = 0; s <= 4; s++) {
                        const y = h - (s / 4) * h;
                        ctx.beginPath();
                        ctx.moveTo(0.5, y + 0.5);
                        ctx.lineTo(w - 0.5, y + 0.5);
                        ctx.stroke();
                    }
                    const colors = [Theme.danger, Theme.success, Theme.kindVideo];
                    const bandW = w / bands.length;
                    for (let b = 0; b < bands.length; b++) {
                        const minArr = bands[b] ? bands[b].min : null;
                        const maxArr = bands[b] ? bands[b].max : null;
                        if (!minArr || !maxArr || minArr.length === 0)
                            continue;
                        const n = minArr.length;
                        const colW = bandW / n;
                        const x0 = b * bandW;
                        ctx.fillStyle = colors[b] || Theme.accent;
                        for (let i = 0; i < n; i++) {
                            // Untouched columns keep the empty sentinel
                            // (min 255, max 0): skip them.
                            if (maxArr[i] < minArr[i])
                                continue;
                            const top = h - maxArr[i] / 255 * h;
                            const bottom = h - minArr[i] / 255 * h;
                            ctx.fillRect(x0 + i * colW, top, Math.max(colW, 1), Math.max(bottom
                                                                                         - top, 1));
                        }
                    }
                }

                // The vectorscope: the Cb/Cr density plane, drawn at the plot's
                // own resolution so a 256x256 grid scales with the pane. Each
                // pixel's alpha tracks its bin's density.
                function drawVectorscope(ctx, w, h) {
                    const dens = panel.snapshot ? panel.snapshot.density : null;
                    const size = panel.snapshot && panel.snapshot.size ? panel.snapshot.size : 256;
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
                    const snap = panel.snapshot;
                    const lumaMin = snap ? snap.lumaMin : null;
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
                    drawChannel(snap.redMin, snap.redMax, Theme.danger, 0.40);
                    drawChannel(snap.greenMin, snap.greenMax, Theme.success, 0.40);
                    drawChannel(snap.blueMin, snap.blueMax, Theme.kindVideo, 0.40);
                    drawChannel(lumaMin, snap.lumaMax, Theme.accent, 0.85);
                    ctx.globalAlpha = 1.0;
                }

                // The plot is a drawing surface, not a control: expose it as a
                // chart named for the selected scope so assistive tech announces
                // what the pane is showing rather than an unlabelled canvas.
                Accessible.name: panel.modes[panel.mode].name + " scope plot"
                Accessible.role: Accessible.Chart
                anchors.fill: parent
                anchors.margins: 4
                visible: !panel.idle

                onPaint: {
                    const ctx = getContext("2d");
                    const w = width, h = height;
                    ctx.clearRect(0, 0, w, h);
                    if (panel.mode === 0)
                        drawHistogram(ctx, w, h);
                    else if (panel.mode === 1)
                        drawWaveform(ctx, w, h);
                    else if (panel.mode === 2)
                        drawVectorscope(ctx, w, h);
                    else
                        drawParade(ctx, w, h);
                }
            }

            // The idle note: shown before a frame arrives and while the scopes
            // are off. Never a blank plot.
            Text {
                anchors.centerIn: parent
                color: Theme.textDisabled
                font.pixelSize: Theme.fsMd
                text: "Scopes idle — waiting for a frame."
                visible: panel.idle
            }
        }
    }
}
