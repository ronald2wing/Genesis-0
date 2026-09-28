// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The shell's theme: every colour, radius, spacing metric and font size the
// interface is made of, read from `Theme` and nowhere else. No other file in
// src/app/qml names a colour (the media-kind and status hues are the exception
// only in that they are *read* here; the values live in this file).
//
// Two palettes - dark and light - picked by one runtime switch (`dark`), and
// an accent spread into its hover/border/wash/ink companions so the accent can
// change at runtime as one colour. The default palette reproduces the shell's
// original hardcoded greys, so a fresh checkout looks exactly as it did
// before; `dark: false` retones to a light equivalent, and `accent` retints
// every selection and activated control.
//
// Deliberately NOT here: i18n, a per-platform title/menu convention, and the
// adaptive phone/tablet layout - those are the shell's, not the theme's.
//
// Registered as a QML singleton via QT_QML_SINGLETON_TYPE in CMakeLists.txt.

pragma Singleton

import QtQuick

QtObject {
    // The accent colour. Default is the shell's original blue; set it to any
    // colour at runtime and every selection/activated control retints, with
    // the three companions derived below rather than hand-paired.
    property color accent: "#3a5aa8"
    // The outline a selected control wears.
    readonly property color accentBorder: Qt.lighter(accent, 1.1)

    // ── accent companions ────────────────────────────────────────────────
    // A step brighter, for a hovered/activated control.
    readonly property color accentHover: Qt.lighter(accent, 1.15)
    // The accent at a wash alpha, for the ground of a selected row.
    readonly property color accentSoft: withAlpha(accent, dark ? 0.20 : 0.18)
    // The amber wash that lights a clip whose media is selected in the bin.
    readonly property color amberWash: "#e0a030"

    // ── clip ink and media kinds ─────────────────────────────────────────
    // These are content colours: a clip's kind mark does not change with the
    // theme, so they are identical under both palettes.
    readonly property color clipInk: dark ? "#101014" : "#ffffff"
    // A raised control (button, toggle) at rest.
    readonly property color control: dark ? "#2a2a34" : "#e8e8ee"
    // A control's outline.
    readonly property color controlBorder: dark ? "#34343f" : "#c9c9d2"
    // A raised control disabled: sunken, so it reads as off.
    readonly property color controlDisabled: dark ? "#1c1c24" : "#f0f0f4"
    readonly property int controlHeight: 30
    // A raised control under a pointer.
    readonly property color controlHover: dark ? "#30303a" : "#eef0f4"
    // A raised control pressed.
    readonly property color controlPressed: dark ? "#3a3a46" : "#d2d2da"
    readonly property color danger: dark ? "#d07a5a" : "#b04530"
    // A desaturated danger ground for a badge/bar, not the vivid mark itself.
    readonly property color dangerWash: dark ? "#6a3030" : "#f5d9d4"
    // ── the switch ───────────────────────────────────────────────────────
    // Which palette the surface/text tokens below read from.
    property bool dark: true

    // ── the type scale ───────────────────────────────────────────────────
    // `font` is the interface face; `fontTechnical` is for timecode, paths
    // and counts - the values you read rather than the language.
    readonly property string font: "sans-serif"
    readonly property string fontTechnical: "monospace"
    readonly property int fs: 11
    readonly property int fsHeading: 20
    readonly property int fsInput: 15
    readonly property int fsLg: 13
    readonly property int fsMd: 12
    readonly property int fsSm: 10
    readonly property int fsTitle: 16
    readonly property int fsXl: 14
    readonly property int fsXs: 9
    readonly property int gap: 6
    readonly property color kindAudio: "#3fae7a"
    readonly property color kindFallback: "#6a6ad4"
    readonly property color kindImage: "#c79a3a"
    readonly property color kindLayer: "#d4655f"
    readonly property color kindText: "#a06ad4"
    readonly property color kindVideo: "#4a7fd4"
    // Ink for text and glyphs sitting ON the accent. Dark ink for a bright
    // accent, light ink for a dark one - perceived lightness, so a green gets
    // dark ink and a blue gets light, as each should.
    readonly property color onAccent: (0.299 * accent.r + 0.587 * accent.g + 0.114 * accent.b)
                                      > 0.55 ? "#16161a" : "#ffffff"
    readonly property int pad: 12

    // ── surfaces ─────────────────────────────────────────────────────────
    // The window ground the panes float on.
    readonly property color page: dark ? "#101014" : "#ececf0"
    // A pane's body.
    readonly property color panel: dark ? "#16161c" : "#ffffff"
    // A pane's outline.
    readonly property color panelBorder: dark ? "#2a2a34" : "#d5d5dc"
    readonly property int radius: 4
    readonly property int radiusLarge: 8
    readonly property int radiusPanel: 6
    readonly property int radiusSmall: 3
    // The alternating zebra stripe a list draws so rows stay countable.
    readonly property color rowAlt: dark ? "#1a1a20" : "#f6f6fa"

    // ── overlay / scrim ──────────────────────────────────────────────────
    // The dim over the shell behind a dialog.
    readonly property color scrim: "#80000000"

    // ── selection ────────────────────────────────────────────────────────
    // The wash a selected bin card / clip-list row sits on.
    readonly property color selectionWash: dark ? "#2a3450" : "#dbe4f5"

    // ── shape and metrics ────────────────────────────────────────────────
    readonly property int spacing: 8

    // ── status ───────────────────────────────────────────────────────────
    readonly property color success: dark ? "#5ad07a" : "#1e8e4e"
    readonly property color textDisabled: dark ? "#5a5a68" : "#b4b4be"

    // ── text ─────────────────────────────────────────────────────────────
    readonly property color textPrimary: dark ? "#e8e8ee" : "#1b1b22"
    readonly property color textSecondary: dark ? "#9a9aa8" : "#5c5c68"
    // A sunken well: a search box, a lane, a header, a text field.
    readonly property color well: dark ? "#1c1c24" : "#f2f2f6"
    // A well's outline.
    readonly property color wellBorder: dark ? "#24242f" : "#dcdce2"

    // `color` with its alpha replaced, used to derive the wash above.
    function withAlpha(c, a) {
        return Qt.rgba(c.r, c.g, c.b, a);
    }
}
