// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The launch screen: shown once at launch, over the shell, listing the recent
// projects from genesis::workspace (the `project.recents` list, kept by the
// ProjectController context property), a new-project action, and a way into
// the shell. Opening a recent folder routes through `project.openProject`,
// which loads the folder's document (or its settings-only manifest) and swaps
// it into the shared Editor; New Project names an empty project at the
// default frame. Both dismiss into the shell, and both leave the current
// project untouched on a bad path.
//
// No poster is drawn: the host recents entry carries no poster (the host has
// no poster cache for projects), so a card is name + path + size/rate, the
// same quiet, text-only fallback for a project with no frame yet.

import QtQuick

Rectangle {
    id: screen

    // The recents list handed to us by ProjectController.
    property var projects: project.recents
    // The saved-cut templates handed to us by ProjectController.
    property var templates: project.templates

    function dismiss() {
        visible = false;
    }

    anchors.fill: parent
    color: Theme.page

    // Named so the host's dev screenshot hook can find and dismiss this
    // surface without a display.
    objectName: "startScreen"
    visible: true
    // Above the menu bar (z:51) so the launch screen covers the whole shell.
    z: 100

    Column {
        id: column

        anchors.centerIn: parent
        spacing: 20
        width: Math.min(parent.width - 160, 560)

        Text {
            color: Theme.textPrimary
            font.bold: true
            font.pixelSize: Theme.fsHeading
            text: i18n.translate("app.title")
        }
        Row {
            spacing: 8
            width: parent.width

            Text {
                color: Theme.textSecondary
                font.pixelSize: Theme.fsLg
                text: i18n.translate("start.recentProjects")
            }
            Text {
                color: Theme.textDisabled
                font.family: Theme.fontTechnical
                font.pixelSize: Theme.fsLg
                text: "(" + screen.projects.length + ")"
            }
        }
        Column {
            spacing: 6
            width: parent.width

            // The empty state: a machine that has never opened a project.
            Text {
                color: Theme.textDisabled
                font.pixelSize: Theme.fsMd
                text: i18n.translate("start.empty")
                visible: screen.projects.length === 0
                width: parent.width
            }
            Repeater {
                model: screen.projects

                delegate: Panel {
                    height: 64
                    radius: Theme.radius
                    width: parent.width

                    Row {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 12

                        Column {
                            spacing: 3
                            width: parent.width - 96

                            Text {
                                color: Theme.textPrimary
                                elide: Text.ElideRight
                                font.pixelSize: Theme.fsLg
                                text: modelData.name
                                width: parent.width
                            }
                            Text {
                                color: Theme.textSecondary
                                font.family: Theme.fontTechnical
                                font.pixelSize: Theme.fsSm
                                text: modelData.width + "×" + modelData.height + " · " + (
                                          modelData.rateDen > 0 ? (modelData.rateNum
                                                                   / modelData.rateDen).toFixed(2) :
                                                                  "0") + " fps"
                            }
                            Text {
                                color: Theme.textDisabled
                                elide: Text.ElideMiddle
                                font.family: Theme.fontTechnical
                                font.pixelSize: Theme.fsXs
                                text: modelData.path
                                width: parent.width
                            }
                        }

                        // Opens the folder; a failed open (missing manifest,
                        // unreadable) leaves the current project alone.
                        Button {
                            anchors.verticalCenter: parent.verticalCenter
                            label: i18n.translate("start.open")

                            onClicked: {
                                if (project.openProject(modelData.path)) {
                                    screen.dismiss();
                                }
                            }
                        }
                    }
                }
            }
        }
        Row {
            spacing: 8
            width: parent.width

            Text {
                color: Theme.textSecondary
                font.pixelSize: Theme.fsLg
                text: i18n.translate("start.templates")
            }
            Text {
                color: Theme.textDisabled
                font.family: Theme.fontTechnical
                font.pixelSize: Theme.fsLg
                text: "(" + screen.templates.length + ")"
            }
        }
        Column {
            spacing: 6
            width: parent.width

            // The empty state: no saved-cut template files under the config
            // directory yet.
            Text {
                color: Theme.textDisabled
                font.pixelSize: Theme.fsMd
                text: i18n.translate("start.templatesEmpty")
                visible: screen.templates.length === 0
                width: parent.width
            }
            Repeater {
                model: screen.templates

                delegate: Panel {
                    height: 48
                    radius: Theme.radius
                    width: parent.width

                    Row {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 12

                        Column {
                            spacing: 3
                            width: parent.width - 96

                            Text {
                                color: Theme.textPrimary
                                elide: Text.ElideRight
                                font.pixelSize: Theme.fsLg
                                text: modelData.name
                                width: parent.width
                            }
                            Text {
                                color: Theme.textDisabled
                                elide: Text.ElideMiddle
                                font.family: Theme.fontTechnical
                                font.pixelSize: Theme.fsXs
                                text: modelData.path
                                width: parent.width
                            }
                        }

                        // Starts a fresh project from the template; a refused
                        // template (missing, unreadable) leaves a fresh empty
                        // project and keeps the start screen up.
                        Button {
                            anchors.verticalCenter: parent.verticalCenter
                            label: i18n.translate("start.create")

                            onClicked: {
                                if (project.newFromTemplate(modelData.path)) {
                                    screen.dismiss();
                                }
                            }
                        }
                    }
                }
            }
        }
        Column {
            spacing: 8
            width: parent.width

            // Names the empty project New Project starts. No folder is
            // created (a location picker is a later concern); the name is the
            // project's display name until it is first saved.
            InputField {
                id: newName

                placeholder: i18n.translate("start.namePlaceholder")
                width: parent.width
            }
            Row {
                spacing: 8
                width: parent.width

                Button {
                    label: i18n.translate("start.newProject")

                    onClicked: {
                        project.newProject(newName.text);
                        screen.dismiss();
                    }
                }
                Button {
                    accent: true
                    label: i18n.translate("start.continue")

                    onClicked: screen.dismiss()
                }
            }
        }
    }
}
