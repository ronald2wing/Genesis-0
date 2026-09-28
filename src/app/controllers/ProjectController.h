// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The project-load facade the start screen drives: it owns the load path (a
// document file or a project folder) and the recents list, and exposes the
// current project's name and path so the header can title itself. The Editor
// it points into is owned by the shell and never recreated, so every controller
// holding that Editor* stays valid across a load; loading only swaps the
// project inside it.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

#include <filesystem>

namespace genesis::project {
class Editor;
}

class ProjectController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString name READ name NOTIFY projectChanged)
    Q_PROPERTY(QString path READ path NOTIFY projectChanged)
    // Where a save lands: the opened document/folder path, empty for a
    // project with no home yet (a fresh New Project, or the launch demo).
    // Empty means Save must route through Save As.
    Q_PROPERTY(QString currentPath READ path NOTIFY projectChanged)
    // Whether the editor holds unsaved changes. Mirrors Editor::dirty and
    // drives the title bar's dirty flag.
    Q_PROPERTY(bool dirty READ dirty NOTIFY dirtyChanged)
    // Whether a save can run at all: true while a project is open. The
    // single-Editor shell always holds one, so this gates the Save item.
    Q_PROPERTY(bool canSave READ canSave NOTIFY projectChanged)
    Q_PROPERTY(QVariantList recents READ recents NOTIFY recentsChanged)
    // The templates discovered under the config directory's `templates`
    // folder, one map per template: { "name": QString, "path": QString }.
    // Sorted by name; empty until a template file exists there.
    Q_PROPERTY(QVariantList templates READ templates NOTIFY templatesChanged)

public:
    ProjectController(genesis::project::Editor *editor, std::filesystem::path config_dir,
                      QObject *parent = nullptr);

    QString name() const;
    QString path() const;
    QVariantList recents() const;
    QVariantList templates() const;
    bool dirty() const;
    bool canSave() const;

    // Opens `path`: a directory is a project folder (its manifest supplies
    // the name and frame), a file is a full document. Returns false and
    // emits loadFailed on an unreadable path, leaving the current project
    // untouched.
    Q_INVOKABLE bool openProject(const QString &path);

    // Resets to a fresh, empty project at the default frame. No folder is
    // created - a location picker is out of scope for this increment - so the
    // project has no path until it is first saved.
    Q_INVOKABLE bool newProject(const QString &name);

    // Starts a fresh project from the template file at `path`, mirroring
    // newProject's reset first so the template's media and timeline land in
    // an empty document, then instantiates it through
    // workspace::instantiate_template (an undoable import). The new project
    // takes the template's display name and, like New Project, has no home
    // until saved. Returns false - leaving a fresh empty project and emitting
    // loadFailed - when the template file is missing or unusable.
    Q_INVOKABLE bool newFromTemplate(const QString &path);

    // Writes the document to the current path (a folder project's manifest,
    // or a standalone document file). Returns false - leaving the project
    // untouched - when there is no path yet, so the shell routes Save to
    // Save As, or when the file cannot be written (saveFailed is emitted).
    Q_INVOKABLE bool save();

    // Writes the document to `path` and adopts it as the current path. The
    // display name is unchanged; the document carries it. Returns false and
    // emits saveFailed when the file cannot be written.
    Q_INVOKABLE bool saveAs(const QString &path);

    // Names the project already in the editor (the demo built at launch),
    // which has no manifest or document of its own.
    void adopt(const QString &name, const QString &path);

signals:
    void projectChanged();
    void recentsChanged();
    void templatesChanged();
    void loadFailed(const QString &message);
    void saveFailed(const QString &message);
    void dirtyChanged();

private:
    bool openFolder(const std::filesystem::path &root);
    bool openDocument(const std::filesystem::path &file);
    // Serializes the project and writes it to `target` (a folder's manifest
    // when `target` is a directory), marking the editor clean on success.
    bool writeTo(const std::filesystem::path &target);
    void setDisplay(const QString &name, const QString &path);
    void refreshRecents();
    // The template folder under the config directory, and its `*.json`
    // contents as { name, path } rows.
    std::filesystem::path templatesRoot() const;
    void refreshTemplates();
    // The display name a template file carries (its document `name`), or the
    // file's stem when it has none.
    std::string templateName(const std::filesystem::path &file) const;

    genesis::project::Editor *editor_ = nullptr;
    std::filesystem::path config_dir_;
    QString name_;
    QString path_;
    QVariantList recents_;
    QVariantList templates_;
};
