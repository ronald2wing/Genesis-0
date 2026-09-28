// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The settings dialog: a dimmed overlay (Panel-based, like ExportDialog)
// editing what the app actually has settings for - the cache directory, the
// decode preference, the scopes poll interval, and the proxy toggle. Every
// live field writes through the AppSettings facade exposed as `settings`,
// which forwards to the real controller or adapter call.
//
// Hand-rolled like the rest of the shell (no QtQuick.Controls): chips are
// ToggleButtons, inputs are InputField, buttons are Button.

import QtQuick

Rectangle {
    id: dialog

    // The decode-preference choices. The controller stores them as 0/1/2
    // (DecodePreference::Auto/Software/Hardware), the same integers here.
    readonly property var decodeModes: [
        {
            name: "Auto",
            value: 0
        },
        {
            name: "Software",
            value: 1
        },
        {
            name: "Hardware",
            value: 2
        }
    ]

    function close() {
        visible = false;
    }
    function open() {
        visible = true;
    }

    anchors.fill: parent
    color: Theme.scrim
    visible: false

    // Block input to the shell behind the dialog.
    MouseArea {
        anchors.fill: parent
    }
    Panel {
        id: panel

        anchors.centerIn: parent
        height: 560
        radius: Theme.radiusLarge
        width: 460

        Column {
            anchors.fill: parent
            anchors.margins: 20
            spacing: 12

            Text {
                color: Theme.textPrimary
                font.bold: true
                font.pixelSize: Theme.fsTitle
                text: "Settings"
            }
            Rectangle {
                color: Theme.panelBorder
                height: 1
                width: parent.width
            }

            // Cache directory: the real setter is MediaBinController::setCacheRoot.
            // Already-generated caches are not migrated; the next cache run (an
            // edit's refresh) uses the new root.
            Column {
                spacing: 4
                width: parent.width

                Text {
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fs
                    text: "Cache directory"
                }
                InputField {
                    text: settings.cacheRoot
                    width: parent.width

                    onEditingFinished: settings.cacheRoot = text
                }
            }

            // Decode preference: EngineSession::set_decode_preference, applied
            // before the next load (the settings facade reloads the timeline).
            Column {
                spacing: 4
                width: parent.width

                Text {
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fs
                    text: "Decode preference"
                }
                Row {
                    spacing: 6

                    Repeater {
                        model: dialog.decodeModes

                        ToggleButton {
                            checked: settings.decodePreference === modelData.value
                            label: modelData.name

                            onClicked: settings.decodePreference = modelData.value
                        }
                    }
                }
            }

            // Scopes interval: ScopesController::setIntervalMs (the worker's
            // poll interval), bridged through the settings facade because the
            // controller does not publish it as a Q_PROPERTY.
            Column {
                spacing: 4
                width: parent.width

                Text {
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fs
                    text: "Scopes interval (ms)"
                }
                InputField {
                    text: String(settings.scopesIntervalMs)
                    width: 90

                    input.validator: IntValidator {
                        bottom: 1
                    }

                    onEditingFinished: {
                        const v = parseInt(text);
                        if (v >= 1)
                            settings.scopesIntervalMs = v;
                    }
                }
            }

            // Proxies: ProxyController::enabled, bridged through the settings
            // facade. Turning it on generates whole-source downscales off the
            // UI thread and re-resolves the preview to read them; turning it
            // off re-resolves back to the originals. Export is unaffected.
            Column {
                spacing: 4
                width: parent.width

                Text {
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fs
                    text: "Use proxies"
                }
                ToggleButton {
                    checked: settings.proxiesEnabled
                    label: settings.proxiesEnabled ? "On" : "Off"

                    onClicked: settings.proxiesEnabled = !settings.proxiesEnabled
                }
            }

            // Updates: the notify-only surface bound to the `updates`
            // controller. "Check now" fetches and verifies the feed and
            // compares versions; nothing is downloaded or applied here -
            // install stays the platform channel's job
            // (docs/decisions/auto-update.md §3(d)).
            Column {
                spacing: 4
                width: parent.width

                Text {
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fs
                    text: "Updates"
                }
                InputField {
                    placeholder: "Update feed URL"
                    text: updates.feedUrl
                    width: parent.width

                    onEditingFinished: updates.feedUrl = text
                }
                Text {
                    color: updates.state === "error" ? Theme.danger : updates.state
                                                       === "update-available" ? Theme.success :
                                                                                Theme.textSecondary
                    font.pixelSize: Theme.fsSm
                    text: updates.statusText
                    visible: updates.statusText !== ""
                    width: parent.width
                    wrapMode: Text.Wrap
                }
                Text {
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fsSm
                    text: updates.releaseNotes
                    visible: updates.state === "update-available" && updates.releaseNotes !== ""
                    width: parent.width
                    wrapMode: Text.Wrap
                }
                Row {
                    layoutDirection: Qt.RightToLeft
                    spacing: 8
                    width: parent.width

                    Button {
                        label: "Check now"

                        onClicked: updates.check()
                    }
                }
            }
            Row {
                layoutDirection: Qt.RightToLeft
                spacing: 8
                width: parent.width

                Button {
                    label: "Close"

                    onClicked: dialog.close()
                }
            }
        }
    }
}
