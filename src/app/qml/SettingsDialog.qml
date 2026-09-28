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

    // The Storage section's inline arm/confirm state for the Clear cache
    // button: the first click arms it, the second (now labelled Confirm)
    // clears. Mirrors ExtensionsDialog's removeArmed pattern.
    property bool clearArmed: false

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

    // The extension slot contributions for the Updates section: every
    // `extensions.slots` entry whose `point` is "settings.updates", already
    // ordered by the host (priority ascending). Rendered below the built-in
    // update controls.
    readonly property var updateSlots: {
        const all = extensions.slots;
        const out = [];
        for (let i = 0; i < all.length; ++i) {
            if (all[i].point === "settings.updates")
                out.push(all[i]);
        }
        return out;
    }

    function close() {
        visible = false;
    }

    // A byte count as a human-readable size (B/KiB/MiB/GiB/TiB), one decimal.
    function formatBytes(bytes) {
        if (bytes < 1024)
            return bytes + " B";
        const units = ["KiB", "MiB", "GiB", "TiB"];
        let value = bytes;
        let i = -1;
        do {
            value /= 1024;
            ++i;
        } while (value >= 1024 && i < units.length - 1)
        return value.toFixed(1) + " " + units[i];
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
                    accessibleName: "Cache directory"
                    focus: true
                    text: settings.cacheRoot
                    width: parent.width

                    onEditingFinished: settings.cacheRoot = text
                }
            }

            // Storage: the processed-media cache's on-disk size and its eviction
            // cap. The size is read-only (computed by the cache controller); the
            // cap is owned and persisted by the settings facade, so the field
            // binds directly to settings.cacheCapBytes. Clear is an inline
            // arm/confirm, the same pattern the extensions dialog uses.
            Column {
                spacing: 6
                width: parent.width

                Text {
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fs
                    text: "Storage"
                }
                Row {
                    spacing: 6

                    Text {
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fs
                        text: "Cache size: " + dialog.formatBytes(cache.sizeBytes)
                        verticalAlignment: Text.AlignVCenter
                    }
                }
                Row {
                    spacing: 6

                    Text {
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fs
                        text: "Cache cap (GiB)"
                        verticalAlignment: Text.AlignVCenter
                    }
                    InputField {
                        accessibleName: "Cache cap in gibibytes"
                        text: String(settings.cacheCapBytes / (1024 * 1024 * 1024))
                        width: 90

                        input.validator: IntValidator {
                            bottom: 1
                        }

                        onEditingFinished: {
                            const v = parseInt(text);
                            if (v >= 1)
                                settings.cacheCapBytes = v * 1024 * 1024 * 1024;
                        }
                    }
                }
                Row {
                    spacing: 6

                    Button {
                        label: dialog.clearArmed ? "Confirm" : "Clear cache"

                        onClicked: {
                            if (dialog.clearArmed) {
                                dialog.clearArmed = false;
                                cache.clearCache();
                            } else {
                                dialog.clearArmed = true;
                            }
                        }
                    }
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
                    accessibleName: "Scopes interval in milliseconds"
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

            // Updates: the check-and-apply surface bound to the `updates`
            // controller. "Check now" fetches and verifies the feed and
            // compares versions; a confirmed "Download & install update"
            // downloads, stages, and hands the swap to a detached helper that
            // replaces the install and relaunches the app
            // (docs/decisions/auto-update.md §10).
            Column {
                spacing: 4
                width: parent.width

                Text {
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fs
                    text: "Updates"
                }
                InputField {
                    accessibleName: "Update feed URL"
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
                    Button {
                        accent: true
                        enabled: updates.state === "update-available"
                        label: "Download & install update"

                        onClicked: updates.apply(true)
                    }
                }
                Text {
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fsSm
                    text: "the app will restart to finish"
                    visible: updates.state === "applying"
                    width: parent.width
                    wrapMode: Text.Wrap
                }

                // The extension slot contributions for this surface, in host
                // order. Each is a Loader that self-sizes to its content; a
                // load failure collapses the Loader and shows a visible error
                // surface instead.
                Repeater {
                    model: dialog.updateSlots

                    Item {
                        id: updateSlotHost

                        height: Math.max(updateSlotLoader.item
                                         ? updateSlotLoader.item.implicitHeight : 0,
                                         updateErrorSurface.visible ? updateErrorSurface.height : 0)
                        width: parent.width

                        Loader {
                            id: updateSlotLoader

                            anchors.left: parent.left
                            anchors.right: parent.right
                            asynchronous: true
                            height: item ? item.implicitHeight : 0
                            source: modelData.url

                            onStatusChanged: {
                                if (status === Loader.Error)
                                    console.warn("extension slot failed to load: " + modelData.id
                                                 + " @ " + modelData.point + " ("
                                                 + updateSlotLoader.source + ")");
                            }
                        }
                        Rectangle {
                            id: updateErrorSurface

                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            border.color: Theme.danger
                            color: Theme.panel
                            height: updateErrorText.implicitHeight + 16
                            radius: Theme.radiusSmall
                            visible: updateSlotLoader.status === Loader.Error

                            Text {
                                id: updateErrorText

                                anchors.centerIn: parent
                                color: Theme.danger
                                font.pixelSize: Theme.fsSm
                                horizontalAlignment: Text.AlignHCenter
                                text: "Slot failed to load: " + modelData.id + "\n"
                                      + updateSlotLoader.source
                                width: parent.width - 16
                                wrapMode: Text.Wrap
                            }
                        }
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
