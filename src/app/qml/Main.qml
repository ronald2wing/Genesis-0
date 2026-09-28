// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The first studio shell: a monitor, a timeline strip, a transport bar, a
// media bin, a clip list and a clip inspector/keyframes pane, all reading the
// host projection (timelineModel), driving the engine-neutral playback
// controller and the edit controller. This increment adds the theme singleton
// (every colour below reads `Theme`), a menu bar (File/Edit/View) and a
// discoverable shortcut set, both routed through one `runAction` so no
// shortcut can bypass the command layer. Editing is live - drag/trim, delete,
// undo/redo, track toggles, snap/ripple and per-clip properties (including
// the applied effect chain and its parameters) - with the title bar showing
// the dirty flag. Deliberately NOT in scope for this increment: multi-select,
// ripple-truncate UI polish, scopes, and the adaptive phone/tablet variants.
// i18n, a tray and per-platform menu conventions are the shell's host
// concerns, not the theme's.

import QtQuick
import QtQuick.Window
import Qt.labs.platform as Platform
import GenesisApp 1.0

Window {
    id: root

    // The Extensions menu's items, derived from the controller's `menus`
    // projection. Each entry becomes a menu item whose `action` token is
    // "ext:<id>:<verb>", where <verb> is the extension's `action` (routed
    // through extensions.invoke) or its `call` (a declarative host call the
    // controller does not yet expose, so it renders disabled with a reason).
    // A "Manage Extensions…" item opens the dialog, like Settings.
    readonly property var extensionMenuItems: {
        const items = [];
        const menus = extensions.menus;
        for (let i = 0; i < menus.length; ++i) {
            const m = menus[i];
            const verb = m.action !== "" ? m.action : m.call;
            items.push({
                           label: m.label,
                           action: "ext:" + m.id + ":" + verb,
                           extId: m.id,
                           extVerb: verb,
                           extIsCall: m.action === ""
                       });
        }
        if (items.length > 0)
            items.push({
                           separator: true
                       });
        items.push({
                       label: "Manage Extensions…",
                       action: "extensions"
                   });
        return items;
    }

    // The menu bar's model: the static File/Edit/View menus plus the dynamic
    // Extensions menu, which rebuilds whenever the controller re-projects.
    readonly property var fullMenuModel: {
        const model = menuModel.slice();
        model.push({
                       title: "Extensions",
                       items: extensionMenuItems
                   });
        return model;
    }

    // Which pane the right column shows below the clip list: the property
    // inspector, the keyframes pane, the scopes, or a contributed extension
    // panel (its token is "ext:<index>", the index into extensions.panels).
    property string inspectorTab: "inspector"
    property bool inspectorVisible: true

    // The extension panels that have been activated at least once, so their
    // Loader instantiates lazily on first activation and stays alive after.
    property var loadedPanels: ({})

    // The three collapsible regions (toggled from the View menu) and which
    // menu is currently dropped open ("" = none).
    property bool mediaBinVisible: true

    // The menu bar's model: titles and their items. `action` is the token
    // runAction/actionEnabled/actionChecked switch on; `shortcut` is a display
    // hint (bound in the Shortcut list below only for real actions);
    // `checkable` draws the tick column; `separator` is a rule, not an item.
    property var menuModel: [
        {
            title: "File",
            items: [
                {
                    label: "Export",
                    shortcut: "Ctrl+E",
                    action: "export"
                },
                {
                    label: "Captions",
                    shortcut: "Ctrl+Shift+C",
                    action: "captions"
                },
                {
                    label: "Cutout",
                    shortcut: "Ctrl+Shift+K",
                    action: "cutout"
                },
                {
                    label: "Enhance",
                    shortcut: "Ctrl+Shift+E",
                    action: "enhance"
                },
                {
                    label: "Speech",
                    shortcut: "Ctrl+Shift+T",
                    action: "speech"
                },
                {
                    label: "Settings",
                    action: "settings"
                },
                {
                    separator: true
                },
                {
                    label: "Save",
                    shortcut: "Ctrl+S",
                    action: "save"
                },
                {
                    label: "Save As",
                    shortcut: "Ctrl+Shift+S",
                    action: "saveAs"
                }
            ]
        },
        {
            title: "Edit",
            items: [
                {
                    label: "Undo",
                    shortcut: "Ctrl+Z",
                    action: "undo"
                },
                {
                    label: "Redo",
                    shortcut: "Ctrl+Shift+Z",
                    action: "redo"
                },
                {
                    separator: true
                },
                {
                    label: "Delete",
                    shortcut: "Del",
                    action: "delete"
                },
                {
                    label: "Select All",
                    shortcut: "Ctrl+A",
                    action: "selectAll"
                }
            ]
        },
        {
            title: "View",
            items: [
                {
                    label: "Media Bin",
                    action: "toggleMediaBin",
                    checkable: true
                },
                {
                    label: "Inspector",
                    action: "toggleInspector",
                    checkable: true
                },
                {
                    label: "Timeline",
                    action: "toggleTimeline",
                    checkable: true
                },
                {
                    separator: true
                },
                {
                    label: "Dark Theme",
                    action: "toggleDark",
                    checkable: true
                }
            ]
        }
    ]
    property string openMenu: ""

    // The clip the overlay paints onto: the selection, or "" when none. The
    // overlay gates its visibility on this, so a deselection dismisses it.
    readonly property string paintClipId: edit.selectedClipId

    // The playhead, polled from the session on the host's rational clock.
    property double position: 0.0

    // Whether the stroke-painting overlay over the monitor is up, toggled from
    // the inspector's CutoutSection "Paint Strokes" toggle.
    property bool strokePaintActive: false

    // Whether a text field holds the keyboard focus. The single-key shortcuts
    // (Space, Delete, Backspace) must not fire while a name/search/path is
    // being typed, so they consult this before acting. TextInput/TextEdit are
    // the only items in this shell that carry `inputMethodHints`.
    readonly property bool textEntryActive: {
        const f = root.activeFocusItem;
        return f !== null && f.inputMethodHints !== undefined;
    }
    property bool timelineVisible: true

    // Which menu actions are checkable, read from the live state they toggle.
    function actionChecked(action) {
        switch (action) {
        case "toggleMediaBin":
            return root.mediaBinVisible;
        case "toggleInspector":
            return root.inspectorVisible;
        case "toggleTimeline":
            return root.timelineVisible;
        case "toggleDark":
            return Theme.dark;
        }
        return false;
    }

    // The reason a disabled action cannot run, shown in place of its shortcut
    // hint. Empty for an enabled action.
    function actionDisabledReason(action) {
        if (action === "captions" && !captions.available)
            return captions.statusText;
        if (action === "cutout" && !cutout.available)
            return cutout.statusText;
        if (action === "enhance" && !enhance.available)
            return enhance.statusText;
        if (action === "speech" && !tts.available)
            return tts.statusText;
        if (action.startsWith("ext:") && extensionActionIsCall(action))
            return "declarative call not wired";
        return "";
    }

    // Which menu actions are enabled. Select All still lacks a command
    // (multi-select is out of scope), so it renders disabled as a
    // discoverability hint but does nothing.
    function actionEnabled(action) {
        switch (action) {
        case "save":
            return project.canSave;
        case "selectAll":
            return false;
            // The caption runtime is a compile-time capability: when it is not
            // built in, the action is disabled and the reason is shown in the
            // item's shortcut slot (see the menu delegate).
        case "captions":
            return captions.available;
        case "cutout":
            return cutout.available;
        case "enhance":
            return enhance.available;
        case "speech":
            return tts.available;
        }
        // A declarative `call` entry has no controller path yet, so it renders
        // disabled with a reason rather than silently doing nothing.
        if (action.startsWith("ext:"))
            return !extensionActionIsCall(action);
        return true;
    }

    // Whether the extension menu token "ext:<id>:<verb>" names a declarative
    // `call` entry (as opposed to an extension `action`).
    function extensionActionIsCall(action) {
        const rest = action.slice(4);
        const sep = rest.indexOf(":");
        if (sep <= 0)
            return false;
        const id = rest.slice(0, sep);
        const verb = rest.slice(sep + 1);
        const menus = extensions.menus;
        for (let i = 0; i < menus.length; ++i) {
            const m = menus[i];
            if (m.id === id && m.action === "" && m.call === verb)
                return true;
        }
        return false;
    }

    // Focus a contributed extension panel by its extension id: select its tab
    // and mark it loaded so the lazy Loader instantiates. Used by the Titles
    // shortcut (Ctrl+T), which now opens the builtin `genesis.titles` panel
    // rather than the removed overlay. A missing panel is a no-op.
    function focusExtensionPanel(id) {
        const panels = extensions.panels;
        for (let i = 0; i < panels.length; ++i) {
            if (panels[i].id === id) {
                root.inspectorTab = "ext:" + i;
                const next = Object.assign({}, root.loadedPanels);
                next[i] = true;
                root.loadedPanels = next;
                return;
            }
        }
    }

    // ── the command layer ────────────────────────────────────────────────
    // Every menu item and every shortcut funnels through runAction, so a key
    // chord and a click cannot disagree about what they do. The controllers
    // below (edit.*, controller.*, exportDialog) own the semantics; this
    // switch only routes.
    function runAction(action) {
        switch (action) {
        case "export":
            exportDialog.open();
            break;
        case "titles":
            root.focusExtensionPanel("genesis.titles");
            break;
        case "captions":
            // The controller owns the refusal (no clip, no audio, runtime
            // disabled); the sheet opens first so the reason is visible.
            captionsDialog.open();
            captions.generateCaptions();
            break;
        case "cutout":
            // The inspector's CutoutSection is the primary affordance; this
            // opens the modal sheet, which presents the same controller
            // state. The subject defaults to Person, matching the section.
            cutoutDialog.open();
            cutout.applyCutout("person");
            break;
        case "enhance":
            // The inspector's EnhanceSection is the primary affordance;
            // this opens the modal sheet, which presents the same
            // controller state. The factor stays where the section left it.
            enhanceDialog.open();
            enhance.applyEnhance();
            break;
        case "speech":
            // The sheet collects the narration text and rate; the run
            // starts only when the user presses Speak, so the text input
            // stays editable until then.
            speechDialog.open();
            break;
        case "settings":
            settingsDialog.open();
            break;
        case "extensions":
            extensionsDialog.open();
            break;
        case "save":
            // A project with no home yet saves through Save As.
            if (project.currentPath !== "")
                project.save();
            else
                saveAsDialog.open();
            break;
        case "saveAs":
            saveAsDialog.open();
            break;
        case "undo":
            edit.undo();
            break;
        case "redo":
            edit.redo();
            break;
        case "delete":
            edit.removeSelected();
            break;
        case "playPause":
            controller.togglePlay();
            break;
        case "toggleMediaBin":
            root.mediaBinVisible = !root.mediaBinVisible;
            break;
        case "toggleInspector":
            root.inspectorVisible = !root.inspectorVisible;
            break;
        case "toggleTimeline":
            root.timelineVisible = !root.timelineVisible;
            break;
        case "toggleDark":
            Theme.dark = !Theme.dark;
            break;
        default:
            // An extension menu item: "ext:<id>:<verb>". The verb is the
            // extension's action, routed through the controller so a
            // mutating action applies through the one Editor exactly like
            // any other edit. A declarative `call` entry has no controller
            // path yet and is disabled (see actionEnabled), so it never
            // reaches here.
            if (action.startsWith("ext:")) {
                const rest = action.slice(4);
                const sep = rest.indexOf(":");
                if (sep > 0) {
                    const id = rest.slice(0, sep);
                    const verb = rest.slice(sep + 1);
                    extensions.invoke(id, verb, "{}");
                }
            } // "selectAll" has no command yet: disabled
            break;
        }
    }

    color: Theme.page
    height: 760
    title: "Genesis-0 studio shell" + (project.dirty ? " *" : "")
    visible: true
    width: 1280

    // ── shortcuts ────────────────────────────────────────────────────────
    // Bound only for the actions that exist. Select All (Ctrl+A) appears in
    // the menu as a disabled hint but is not bound here.
    Shortcut {
        sequence: "Ctrl+Z"

        onActivated: root.runAction("undo")
    }
    Shortcut {
        sequence: "Ctrl+Shift+Z"

        onActivated: root.runAction("redo")
    }
    Shortcut {
        sequence: "Ctrl+E"

        onActivated: root.runAction("export")
    }
    Shortcut {
        sequence: "Ctrl+T"

        onActivated: root.runAction("titles")
    }
    Shortcut {
        sequence: "Ctrl+Shift+C"

        onActivated: root.runAction("captions")
    }
    Shortcut {
        sequence: "Ctrl+Shift+K"

        onActivated: root.runAction("cutout")
    }
    Shortcut {
        sequence: "Ctrl+Shift+E"

        onActivated: root.runAction("enhance")
    }
    Shortcut {
        sequence: "Ctrl+Shift+T"

        onActivated: root.runAction("speech")
    }
    Shortcut {
        sequence: "Ctrl+S"

        onActivated: root.runAction("save")
    }
    Shortcut {
        sequence: "Ctrl+Shift+S"

        onActivated: root.runAction("saveAs")
    }
    Shortcut {
        sequence: "Space"

        onActivated: {
            if (!root.textEntryActive)
                root.runAction("playPause");
        }
    }
    Shortcut {
        sequence: "Delete"

        onActivated: {
            if (!root.textEntryActive)
                root.runAction("delete");
        }
    }
    Shortcut {
        sequence: "Backspace"

        onActivated: {
            if (!root.textEntryActive)
                root.runAction("delete");
        }
    }

    // ── menu bar ─────────────────────────────────────────────────────────
    Row {
        id: menuBar

        anchors.left: parent.left
        anchors.leftMargin: 16
        anchors.top: parent.top
        anchors.topMargin: 16
        height: 30
        spacing: 2
        z: 51

        Repeater {
            model: root.fullMenuModel

            delegate: Item {
                id: menuEntry

                height: menuBar.height
                width: titleBox.width

                Rectangle {
                    id: titleBox

                    color: root.openMenu === modelData.title ? Theme.controlPressed : (
                                                                   titleHover.containsMouse
                                                                   ? Theme.controlHover :
                                                                     "transparent")
                    height: parent.height
                    radius: Theme.radiusSmall
                    width: titleText.implicitWidth + 16

                    Text {
                        id: titleText

                        anchors.centerIn: parent
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fsLg
                        text: modelData.title
                    }
                    MouseArea {
                        id: titleHover

                        anchors.fill: parent
                        hoverEnabled: true

                        onClicked: root.openMenu = (root.openMenu === modelData.title) ? "" :
                                                                                         modelData.title
                    }
                }
                Rectangle {
                    id: dropdown

                    border.color: Theme.panelBorder
                    color: Theme.panel
                    height: itemColumn.height + 8
                    radius: Theme.radius
                    visible: root.openMenu === modelData.title
                    width: itemColumn.width + 12
                    x: 0
                    y: parent.height

                    Column {
                        id: itemColumn

                        anchors.left: parent.left
                        anchors.leftMargin: 4
                        anchors.top: parent.top
                        anchors.topMargin: 4
                        spacing: 2

                        Repeater {
                            model: modelData.items

                            delegate: Item {
                                id: menuItem

                                property bool itemChecked: modelData.checkable === true
                                                           && root.actionChecked(modelData.action)
                                property bool itemEnabled: root.actionEnabled(modelData.action)

                                height: modelData.separator ? 9 : 24
                                width: 200

                                Rectangle {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: Theme.panelBorder
                                    height: 1
                                    visible: modelData.separator === true
                                    width: parent.width - 8
                                }
                                Rectangle {
                                    anchors.fill: parent
                                    color: itemHover.containsMouse && menuItem.itemEnabled
                                           ? Theme.selectionWash : "transparent"
                                    radius: Theme.radiusSmall
                                    visible: modelData.separator !== true
                                }
                                Text {
                                    anchors.left: parent.left
                                    anchors.leftMargin: 6
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: Theme.textPrimary
                                    font.pixelSize: Theme.fs
                                    text: "✓"
                                    visible: modelData.separator !== true && menuItem.itemChecked
                                }
                                Text {
                                    anchors.left: parent.left
                                    anchors.leftMargin: 22
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: menuItem.itemEnabled ? Theme.textPrimary :
                                                                  Theme.textDisabled
                                    font.pixelSize: Theme.fs
                                    text: modelData.label
                                    visible: modelData.separator !== true
                                }
                                Text {
                                    anchors.right: parent.right
                                    anchors.rightMargin: 8
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: menuItem.itemEnabled ? Theme.textSecondary :
                                                                  Theme.textDisabled
                                    font.family: Theme.fontTechnical
                                    font.pixelSize: Theme.fs
                                    // A disabled action shows why in place of
                                    // its shortcut hint, so the reason is
                                    // discoverable without a tooltip.
                                    text: {
                                        const reason = root.actionDisabledReason(modelData.action);
                                        if (reason.length > 0)
                                            return reason;
                                        return modelData.shortcut !== undefined
                                                ? modelData.shortcut : "";
                                    }
                                    // Shown for a shortcut hint, or for a
                                    // disabled action's reason even when it has
                                    // no shortcut (an extension `call` entry).
                                    visible: modelData.separator !== true && (modelData.shortcut
                                                                              !== undefined
                                                                              || root.actionDisabledReason(
                                                                                  modelData.action).length
                                                                              > 0)
                                }
                                MouseArea {
                                    id: itemHover

                                    anchors.fill: parent
                                    enabled: menuItem.itemEnabled
                                    hoverEnabled: true
                                    visible: modelData.separator !== true

                                    onClicked: {
                                        root.openMenu = "";
                                        root.runAction(modelData.action);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // While a menu is open this transparent layer swallows the first click
    // anywhere under the bar (z above the shell, below the bar) and closes it.
    MouseArea {
        anchors.fill: parent
        visible: root.openMenu !== ""
        z: 50

        onClicked: root.openMenu = ""
    }

    // The Export quick-action, top-right of the bar row.
    Button {
        accent: true
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.top: parent.top
        anchors.topMargin: 16
        label: "Export"
        z: 51

        onClicked: {
            root.openMenu = "";
            root.runAction("export");
        }
    }

    // Header
    Column {
        id: header

        anchors.left: parent.left
        anchors.leftMargin: 16
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.top: menuBar.bottom
        anchors.topMargin: 12
        spacing: 8

        Text {
            color: Theme.textPrimary
            font.bold: true
            font.pixelSize: Theme.fsHeading
            text: "Genesis-0 — " + project.name
        }
        Rectangle {
            color: Theme.panelBorder
            height: 1
            width: parent.width
        }
        Text {
            color: Theme.textSecondary
            elide: Text.ElideMiddle
            font.pixelSize: Theme.fsMd
            text: project.path
            width: parent.width
        }
    }

    // Inspector pane (right): the flattened clip list above the selected
    // clip's properties. Selecting a clip in either the list or the strip
    // drives the property editor.
    Column {
        id: inspector

        anchors.bottom: timelineWrapper.top
        anchors.bottomMargin: 16
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.top: header.bottom
        anchors.topMargin: 16
        spacing: 16
        visible: root.inspectorVisible
        width: root.inspectorVisible ? 300 : 0

        ClipList {
            height: 220
            tracks: timelineModel.tracks
            width: parent.width
        }

        // Tab bar: Inspector or Keyframes. Selecting a clip in either the
        // list or the strip feeds whichever pane is showing.
        Row {
            id: inspectorTabs

            spacing: 6
            width: parent.width

            ToggleButton {
                checked: root.inspectorTab === "inspector"
                label: "Inspector"

                onClicked: root.inspectorTab = "inspector"
            }
            ToggleButton {
                checked: root.inspectorTab === "keyframes"
                label: "Keyframes"

                onClicked: root.inspectorTab = "keyframes"
            }
            ToggleButton {
                checked: root.inspectorTab === "scopes"
                label: "Scopes"

                onClicked: root.inspectorTab = "scopes"
            }

            // One tab per contributed extension panel. The token is
            // "ext:<index>", the index into extensions.panels; activating it
            // marks the panel loaded so its Loader below instantiates.
            Repeater {
                model: extensions.panels

                ToggleButton {
                    checked: root.inspectorTab === "ext:" + index
                    label: modelData.label

                    onClicked: {
                        root.inspectorTab = "ext:" + index;
                        const next = Object.assign({}, root.loadedPanels);
                        next[index] = true;
                        root.loadedPanels = next;
                    }
                }
            }
        }
        Item {
            height: parent.height - 220 - 16 - inspectorTabs.height - 16
            width: parent.width

            ClipInspector {
                anchors.fill: parent
                paintActive: root.strokePaintActive
                tracks: timelineModel.tracks
                visible: root.inspectorTab === "inspector"

                onPaintToggleRequested: root.strokePaintActive = !root.strokePaintActive
            }
            KeyframesPane {
                anchors.fill: parent
                position: root.position
                tracks: timelineModel.tracks
                visible: root.inspectorTab === "keyframes"
            }
            ScopesPane {
                anchors.fill: parent
                visible: root.inspectorTab === "scopes"
            }

            // One lazy Loader per contributed extension panel. The Loader
            // instantiates on first activation (loadedPanels[index]) and stays
            // alive after, so switching tabs does not reload foreign QML. The
            // panel is wrapped in the design-system shell (a Panel) so a
            // foreign panel that ignores the theme still sits on the shell's
            // surface; a load failure shows a visible message and is logged
            // through the controller's log sink.
            Repeater {
                model: extensions.panels

                Item {
                    id: panelHost

                    anchors.fill: parent
                    visible: root.inspectorTab === "ext:" + index

                    Loader {
                        id: panelLoader

                        active: root.loadedPanels[index] === true
                        anchors.fill: parent
                        asynchronous: true
                        // The controller projects the panel's absolute file URL
                        // in `url`; that is the source the loader opens. The
                        // bare `qml` filename is only a fallback for a
                        // projection that predates `url`.
                        source: modelData.url !== undefined && modelData.url !== "" ? modelData.url :
                                                                                      modelData.qml

                        onStatusChanged: {
                            if (status === Loader.Error) {
                                // The controller's log sink is not exposed to
                                // QML, so the failure is logged to the console
                                // (which the run log captures) with the id and
                                // the URL that failed.
                                console.warn("extension panel failed to load: " + modelData.id
                                             + " / " + panelLoader.source);
                            }
                        }
                    }

                    // The shell around the foreign panel: a Panel body with a
                    // message, so a panel that draws nothing (or fails) still
                    // reads as a pane on the design-system surface. A load
                    // failure shows the id and the URL that failed, in the
                    // danger colour, so the reason is visible without the log.
                    Rectangle {
                        anchors.fill: parent
                        border.color: panelLoader.status === Loader.Error ? Theme.danger :
                                                                            Theme.panelBorder
                        color: Theme.panel
                        radius: Theme.radiusPanel
                        visible: panelLoader.status !== Loader.Ready

                        Text {
                            anchors.centerIn: parent
                            color: panelLoader.status === Loader.Error ? Theme.danger :
                                                                         Theme.textSecondary
                            font.pixelSize: Theme.fsMd
                            horizontalAlignment: Text.AlignHCenter
                            text: panelLoader.status === Loader.Error ? "Panel failed to load: "
                                                                        + modelData.label + "\n"
                                                                        + modelData.id + "\n"
                                                                        + panelLoader.source :
                                                                        "Loading "
                                                                        + modelData.label + "…"
                            width: parent.width - 24
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }
        }
    }

    // The media bin (left): a pane of cards for the imported files, with
    // search/sort and the selection the strip highlights. Reads the
    // TimelineModel projection and the MediaBinController's cache lookups.
    MediaBin {
        id: mediaBinPane

        anchors.bottom: timelineWrapper.top
        anchors.bottomMargin: 16
        anchors.left: parent.left
        anchors.leftMargin: 16
        anchors.top: header.bottom
        anchors.topMargin: 16
        visible: root.mediaBinVisible
        width: root.mediaBinVisible ? 260 : 0
    }

    // The monitor fills what the bin and the inspector leave. Frames keep
    // presenting; this shell only adds panes around it.
    MonitorItem {
        id: video

        anchors.bottom: timelineWrapper.top
        anchors.bottomMargin: 16
        anchors.left: mediaBinPane.right
        anchors.leftMargin: 16
        anchors.right: inspector.left
        anchors.rightMargin: 16
        anchors.top: header.bottom
        anchors.topMargin: 16
    }

    // The stroke-painting overlay, drawn above the monitor (z: 40, below the
    // menu bar's 51) and shown only while the toggle is on and a clip is
    // selected. It commits each stroke through the edit controller, so the
    // monitor's frame stays live underneath while the user corrects the mask.
    CutoutStrokeOverlay {
        id: strokeOverlay

        anchors.fill: video
        clipId: root.paintClipId
        position: root.position
        visible: root.strokePaintActive && root.paintClipId !== ""
        z: 40
    }

    // Bottom area: transport + readout above the timeline strip. Wrapped so
    // the whole region can collapse (its children have intrinsic heights, so
    // the wrapper's height + clip is what removes it from the layout).
    Item {
        id: timelineWrapper

        anchors.bottom: parent.bottom
        anchors.bottomMargin: root.timelineVisible ? 16 : 0
        anchors.left: parent.left
        anchors.right: parent.right
        clip: true
        height: root.timelineVisible ? transportArea.height : 0

        Column {
            id: transportArea

            anchors.bottom: parent.bottom
            spacing: 10
            width: parent.width

            TransportBar {
                playing: controller.playing
                position: root.position
                width: parent.width

                onToggled: controller.togglePlay()
            }
            TimelineStrip {
                position: root.position
                width: parent.width

                onSeekRequested: seconds => controller.seek(seconds)
            }
            Text {
                color: video.rendered > 0 ? Theme.success : Theme.danger
                font.pixelSize: Theme.fsMd
                text: "frames rendered: " + video.rendered + "  ·  frame: " + video.frameInfo
            }
        }

        // The GPU status pill, top-right of the transport strip, aligned with
        // the transport bar's row (the bar packs its controls left, so this
        // floats over the free right edge). Its hover detail drops downward and
        // stays inside the wrapper's clip.
        GpuIndicator {
            anchors.right: parent.right
            anchors.top: parent.top
        }
    }
    Timer {
        interval: 100
        repeat: true
        running: true

        onTriggered: root.position = controller.position
    }

    // The export overlay, drawn last so it covers the shell. Bound to the
    // `exporter` context property; opened by the Export button above.
    ExportDialog {
        id: exportDialog
    }

    // The settings overlay, opened from the File menu. Bound to the `settings`
    // facade; only fields with a real backing API are editable.
    SettingsDialog {
        id: settingsDialog
    }

    // The extensions overlay, opened from the Extensions menu. Bound to the
    // `extensions` controller, which owns the loaded projection, the consent
    // switch and the refusal count; the sheet only presents them.
    ExtensionsDialog {
        id: extensionsDialog
    }

    // The captions overlay, opened from the File menu. Bound to the `captions`
    // controller, which owns the consent gate, the worker and the error text;
    // the sheet only presents them.
    CaptionsDialog {
        id: captionsDialog
    }

    // The cutout overlay, opened from the File menu. Bound to the `cutout`
    // controller, which owns the consent gate, the worker and the error text;
    // the sheet only presents them. The inspector's CutoutSection is the
    // always-visible affordance over the same controller.
    CutoutDialog {
        id: cutoutDialog
    }

    // The enhance overlay, opened from the File menu. Bound to the `enhance`
    // controller, which owns the consent gate, the worker and the error text;
    // the sheet only presents them. The inspector's EnhanceSection is the
    // always-visible affordance over the same controller.
    EnhanceDialog {
        id: enhanceDialog
    }

    // The speech overlay, opened from the File menu. Bound to the `tts`
    // controller, which owns the consent gate, the worker and the error text;
    // the sheet collects the narration text and rate and only presents the
    // controller's state.
    SpeechDialog {
        id: speechDialog
    }

    // The transient success notice the captions sheet raises after a run lands
    // Done. A short-lived banner at the top of the shell; the timer clears it.
    Rectangle {
        id: captionsNotice

        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 56
        border.color: Theme.success
        color: Theme.panel
        height: 30
        radius: Theme.radius
        visible: captionsDialog.notice.length > 0
        width: noticeText.implicitWidth + 24
        z: 95

        Text {
            id: noticeText

            anchors.centerIn: parent
            color: Theme.success
            font.pixelSize: Theme.fsMd
            text: captionsDialog.notice
        }
        Timer {
            interval: 2500
            running: captionsNotice.visible

            onTriggered: captionsDialog.notice = ""
        }
    }

    // The transient success notice the cutout sheet raises after a run lands
    // Done. Sits just below the captions notice so the two never overlap.
    Rectangle {
        id: cutoutNotice

        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 92
        border.color: Theme.success
        color: Theme.panel
        height: 30
        radius: Theme.radius
        visible: cutoutDialog.notice.length > 0
        width: cutoutNoticeText.implicitWidth + 24
        z: 95

        Text {
            id: cutoutNoticeText

            anchors.centerIn: parent
            color: Theme.success
            font.pixelSize: Theme.fsMd
            text: cutoutDialog.notice
        }
        Timer {
            interval: 2500
            running: cutoutNotice.visible

            onTriggered: cutoutDialog.notice = ""
        }
    }

    // The transient success notice the enhance sheet raises after a run lands
    // Done. Sits just below the cutout notice so the three never overlap.
    Rectangle {
        id: enhanceNotice

        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 128
        border.color: Theme.success
        color: Theme.panel
        height: 30
        radius: Theme.radius
        visible: enhanceDialog.notice.length > 0
        width: enhanceNoticeText.implicitWidth + 24
        z: 95

        Text {
            id: enhanceNoticeText

            anchors.centerIn: parent
            color: Theme.success
            font.pixelSize: Theme.fsMd
            text: enhanceDialog.notice
        }
        Timer {
            interval: 2500
            running: enhanceNotice.visible

            onTriggered: enhanceDialog.notice = ""
        }
    }

    // The transient success notice the speech sheet raises after a run lands
    // Done. Sits just below the enhance notice so the four never overlap.
    Rectangle {
        id: speechNotice

        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 164
        border.color: Theme.success
        color: Theme.panel
        height: 30
        radius: Theme.radius
        visible: speechDialog.notice.length > 0
        width: speechNoticeText.implicitWidth + 24
        z: 95

        Text {
            id: speechNoticeText

            anchors.centerIn: parent
            color: Theme.success
            font.pixelSize: Theme.fsMd
            text: speechDialog.notice
        }
        Timer {
            interval: 2500
            running: speechNotice.visible

            onTriggered: speechDialog.notice = ""
        }
    }

    // The Save As overlay, opened from the File menu (and by Save when the
    // project has no home yet). A typed path like the export dialog's output
    // field - the shell has no native file picker - committed through the
    // controller's saveAs; a failed write leaves the dialog open.
    Rectangle {
        id: saveAsDialog

        function close() {
            visible = false;
        }
        function open() {
            saveAsPath.text = project.currentPath;
            visible = true;
        }

        anchors.fill: parent
        color: Theme.scrim
        visible: false
        z: 90

        // Block input to the shell behind while the dialog is open.
        MouseArea {
            anchors.fill: parent
        }
        Panel {
            anchors.centerIn: parent
            height: 200
            radius: Theme.radiusLarge
            width: 460

            Column {
                anchors.fill: parent
                anchors.margins: 20
                spacing: 10

                Text {
                    color: Theme.textPrimary
                    font.bold: true
                    font.pixelSize: Theme.fsTitle
                    text: "Save As"
                }
                Rectangle {
                    color: Theme.panelBorder
                    height: 1
                    width: parent.width
                }
                Text {
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fs
                    text: "Document path"
                }
                InputField {
                    id: saveAsPath

                    placeholder: "/path/to/project.json"
                    width: parent.width
                }
                Row {
                    layoutDirection: Qt.RightToLeft
                    spacing: 8
                    width: parent.width

                    Button {
                        accent: true
                        enabled: saveAsPath.text !== ""
                        label: "Save"

                        onClicked: {
                            if (project.saveAs(saveAsPath.text))
                                saveAsDialog.close();
                        }
                    }
                    Button {
                        label: "Cancel"

                        onClicked: saveAsDialog.close()
                    }
                }
            }
        }
    }

    // The system tray icon: present only where a tray exists (`available`),
    // with a Show action that resurfaces the window and a Quit that leaves
    // the session cleanly.
    Platform.SystemTrayIcon {
        id: tray

        tooltip: i18n.translate("app.title")
        visible: available

        menu: Platform.Menu {
            Platform.MenuItem {
                text: i18n.translate("tray.show")

                onTriggered: {
                    root.show();
                    root.raise();
                    root.requestActivate();
                }
            }
            Platform.MenuSeparator {
            }
            Platform.MenuItem {
                text: i18n.translate("tray.quit")

                onTriggered: Qt.quit()
            }
        }
    }

    // The launch screen, shown once at launch over the shell (z:100, above the
    // menu bar); "Continue to Editor" dismisses it.
    StartScreen {
        id: startScreen
    }
}
