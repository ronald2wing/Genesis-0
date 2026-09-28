// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/controllers/ProjectController.h"

#include <algorithm>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "app/support/AppLog.h"
#include "core/JsonGuard.h"
#include "project/Document.h"
#include "project/Editor.h"
#include "project/Project.h"
#include "project/model/VideoSettings.h"
#include "workspace/Projects.h"
#include "workspace/Templates.h"

using namespace genesis::project;

ProjectController::ProjectController(Editor *editor, std::filesystem::path config_dir,
                                     QObject *parent)
    : QObject(parent), editor_(editor), config_dir_(std::move(config_dir))
{
    refreshRecents();
    refreshTemplates();
}

QString ProjectController::name() const
{
    return name_;
}
QString ProjectController::path() const
{
    return path_;
}
QVariantList ProjectController::recents() const
{
    return recents_;
}
QVariantList ProjectController::templates() const
{
    return templates_;
}

bool ProjectController::dirty() const
{
    return editor_ != nullptr && editor_->dirty();
}

bool ProjectController::canSave() const
{
    return editor_ != nullptr;
}

bool ProjectController::openProject(const QString &path)
{
    const std::filesystem::path target(path.toStdString());
    if (std::filesystem::is_directory(target)) {
        return openFolder(target);
    }
    return openDocument(target);
}

bool ProjectController::newProject(const QString &name)
{
    editor_->load(Project::with_video(VideoSettings{ }));
    const std::string trimmed = name.trimmed().toStdString();
    setDisplay(QString::fromStdString(trimmed.empty() ? "Untitled project" : trimmed), QString());
    return true;
}

bool ProjectController::newFromTemplate(const QString &path)
{
    const std::filesystem::path file(path.toStdString());
    // A template always starts a fresh project, the same reset New Project
    // does, so the template's media and timeline land in an empty document
    // rather than on top of the demo or whatever is open.
    editor_->load(Project::with_video(VideoSettings{ }));
    const auto created = genesis::workspace::instantiate_template(*editor_, file);
    if (!created) {
        // The template was refused; the editor already holds a fresh empty
        // project. Reflect it so the shell behind the start screen agrees,
        // then surface the reason for the log.
        setDisplay(QStringLiteral("Untitled project"), QString());
        emit dirtyChanged();
        emit loadFailed(QString::fromStdString(created.error()));
        return false;
    }
    // Name the new project after the template; it has no home until saved, so
    // Save routes through Save As like any other untitled project.
    setDisplay(QString::fromStdString(templateName(file)), QString());
    emit dirtyChanged();
    return true;
}

bool ProjectController::save()
{
    if (path_.isEmpty()) {
        // No home yet: the shell routes Save to Save As rather than saving
        // into nowhere.
        return false;
    }
    return writeTo(std::filesystem::path(path_.toStdString()));
}

bool ProjectController::saveAs(const QString &path)
{
    if (!writeTo(std::filesystem::path(path.toStdString()))) {
        return false;
    }
    // Adopt the new home; the display name is unchanged and already carried
    // in the document's `name` field.
    setDisplay(name_, path);
    return true;
}

bool ProjectController::writeTo(const std::filesystem::path &target)
{
    // A directory is a project folder whose manifest doubles as the
    // document; anything else is a standalone document file.
    const std::filesystem::path file = std::filesystem::is_directory(target)
            ? genesis::workspace::manifest_path(target)
            : target;

    DocumentSettings settings;
    settings.name = name_.toStdString();
    const nlohmann::json doc = to_document(editor_->project(), settings);

    std::ofstream out(file);
    if (!out) {
        emit saveFailed(QStringLiteral("could not write ") + QString::fromStdString(file.string()));
        return false;
    }
    out << doc.dump(2);
    if (!out) {
        emit saveFailed(QStringLiteral("could not write ") + QString::fromStdString(file.string()));
        return false;
    }
    editor_->mark_saved();
    genesis::app::app_log("[app] saved %s\n", file.string().c_str());
    emit dirtyChanged();
    return true;
}

void ProjectController::adopt(const QString &name, const QString &path)
{
    setDisplay(name, path);
}

bool ProjectController::openFolder(const std::filesystem::path &root)
{
    auto opened = genesis::workspace::open_project(root);
    if (!opened) {
        emit loadFailed(QString::fromStdString(opened.error()));
        return false;
    }
    // The manifest doubles as the document: a folder with a saved edit
    // reopens with its timeline, while a settings-only manifest (a fresh
    // project) parses to no usable timeline and the project starts empty
    // at the manifest's frame.
    std::optional<Project> loaded;
    if (std::ifstream in(genesis::workspace::manifest_path(root)); in) {
        std::string error;
        const std::optional<nlohmann::json> parsed = genesis::core::parse_document_json(in, error);
        if (parsed) {
            loaded = from_document(*parsed);
        }
    }
    if (loaded) {
        editor_->load(std::move(*loaded));
    } else {
        VideoSettings video;
        video.width = opened->width;
        video.height = opened->height;
        video.rate_num = opened->rate_num;
        video.rate_den = opened->rate_den;
        editor_->load(Project::with_video(video));
    }
    genesis::workspace::add_recents(config_dir_, *opened);
    refreshRecents();
    setDisplay(QString::fromStdString(opened->name), QString::fromStdString(opened->path));
    return true;
}

bool ProjectController::openDocument(const std::filesystem::path &file)
{
    std::ifstream in(file);
    if (!in) {
        emit loadFailed(QStringLiteral("could not read ") + QString::fromStdString(file.string()));
        return false;
    }
    std::string error;
    const std::optional<nlohmann::json> parsed = genesis::core::parse_document_json(in, error);
    if (!parsed) {
        emit loadFailed(QString::fromStdString(file.string()) + QStringLiteral(" is not JSON"));
        return false;
    }
    const nlohmann::json &doc = *parsed;
    if (const std::optional<int> version = document_version(doc);
        version && *version > DOCUMENT_VERSION) {
        emit loadFailed(QString::fromStdString(file.string())
                        + QStringLiteral(" was saved by a newer build than this one"));
        return false;
    }
    std::optional<Project> loaded = from_document(doc);
    if (!loaded) {
        emit loadFailed(QString::fromStdString(file.string())
                        + QStringLiteral(" holds a document this build cannot read"));
        return false;
    }
    editor_->load(std::move(*loaded));
    // The name lives in the document's settings, not the Project model;
    // fall back to the file's stem for a document that did not name one.
    std::string name = doc.value("name", std::string());
    if (name.empty()) {
        name = file.stem().string();
    }
    setDisplay(QString::fromStdString(name), QString::fromStdString(file.string()));
    return true;
}

void ProjectController::setDisplay(const QString &name, const QString &path)
{
    if (name_ == name && path_ == path) {
        return;
    }
    name_ = name;
    path_ = path;
    emit projectChanged();
}

void ProjectController::refreshRecents()
{
    recents_.clear();
    for (const genesis::workspace::ProjectInfo &project :
         genesis::workspace::list_recents(config_dir_)) {
        QVariantMap entry;
        entry.insert(QStringLiteral("name"), QString::fromStdString(project.name));
        entry.insert(QStringLiteral("path"), QString::fromStdString(project.path));
        entry.insert(QStringLiteral("width"), static_cast<quint32>(project.width));
        entry.insert(QStringLiteral("height"), static_cast<quint32>(project.height));
        entry.insert(QStringLiteral("rateNum"), static_cast<qlonglong>(project.rate_num));
        entry.insert(QStringLiteral("rateDen"), static_cast<qlonglong>(project.rate_den));
        entry.insert(QStringLiteral("pinned"), project.pinned);
        recents_.append(entry);
    }
    emit recentsChanged();
}

std::filesystem::path ProjectController::templatesRoot() const
{
    return config_dir_ / "templates";
}

std::string ProjectController::templateName(const std::filesystem::path &file) const
{
    std::ifstream in(file);
    if (!in) {
        return file.stem().string();
    }
    std::string error;
    const std::optional<nlohmann::json> parsed = genesis::core::parse_document_json(in, error);
    if (!parsed) {
        return file.stem().string();
    }
    const std::string name = parsed->value("name", std::string());
    return name.empty() ? file.stem().string() : name;
}

void ProjectController::refreshTemplates()
{
    templates_.clear();
    // The template folder may not exist yet (nothing has saved a template);
    // a missing or unreadable root reads as no templates, not an error.
    std::error_code error;
    const std::filesystem::directory_iterator it(templatesRoot(), error);
    if (error) {
        emit templatesChanged();
        return;
    }
    std::vector<std::pair<std::string, std::string>> entries;
    for (const std::filesystem::directory_entry &entry : it) {
        std::error_code file_error;
        if (!entry.is_regular_file(file_error) || entry.path().extension() != ".json") {
            continue;
        }
        entries.emplace_back(templateName(entry.path()), entry.path().string());
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto &left, const auto &right) { return left.first < right.first; });
    for (const auto &[name, file] : entries) {
        QVariantMap row;
        row.insert(QStringLiteral("name"), QString::fromStdString(name));
        row.insert(QStringLiteral("path"), QString::fromStdString(file));
        templates_.append(row);
    }
    emit templatesChanged();
}
