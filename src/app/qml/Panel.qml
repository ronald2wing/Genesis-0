// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// A panel surface: the bordered, rounded body every pane in the shell draws.
// Replaces the hand-rolled `Rectangle { color: "#16161c"; border.color:
// "#2a2a34"; radius: 6 }` repeated across MediaBin, ClipList, ClipInspector,
// TimelineStrip, KeyframesPane and ExportDialog. Callers may override `radius`
// (the export sheet uses Theme.radiusLarge) but nothing else names a colour.

import QtQuick

Rectangle {
    border.color: Theme.panelBorder
    color: Theme.panel
    radius: Theme.radiusPanel
}
