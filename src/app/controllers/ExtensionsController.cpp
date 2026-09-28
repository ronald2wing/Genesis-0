// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/controllers/ExtensionsController.h"

#include <QFile>
#include <QSaveFile>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>

#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include "api/Codec.h"
#include "app/support/AppLog.h"
#include "extensions/Consent.h"
#include "extensions/DisabledList.h"
#include "extensions/ExtensionHost.h"
#include "extensions/Install.h"
#include "extensions/Manager.h"
#include "extensions/RemovedList.h"
#include "extensions/Seed.h"
#include "project/Editor.h"

namespace {

// The log sink the extension host re-prefixes per extension; the shell routes
// extension log lines to the run log.
void extension_log(const std::string &message)
{
    genesis::app::app_log("%s\n", message.c_str());
}

// The origin an extension record came from, as the QML projection spells it.
std::string origin_name(genesis::extensions::Origin origin)
{
    switch (origin) {
    case genesis::extensions::Origin::Builtin:
        return "builtin";
    case genesis::extensions::Origin::Curated:
        return "curated";
    case genesis::extensions::Origin::Developer:
        return "developer";
    case genesis::extensions::Origin::User:
        return "user";
    }
    return "developer";
}

// The origin an install request names, or nullopt for an unknown spelling.
// Mirrors the CLI's parse_origin so the app and the CLI accept the same words.
std::optional<genesis::extensions::Origin> parse_origin(const std::string &name)
{
    if (name == "builtin") {
        return genesis::extensions::Origin::Builtin;
    }
    if (name == "curated") {
        return genesis::extensions::Origin::Curated;
    }
    if (name == "developer") {
        return genesis::extensions::Origin::Developer;
    }
    if (name == "user") {
        return genesis::extensions::Origin::User;
    }
    return std::nullopt;
}

// The load state of an extension record, as the QML projection spells it.
std::string state_name(genesis::extensions::ExtensionState state)
{
    switch (state) {
    case genesis::extensions::ExtensionState::Enabled:
        return "enabled";
    case genesis::extensions::ExtensionState::Disabled:
        return "disabled";
    case genesis::extensions::ExtensionState::Failed:
        return "failed";
    }
    return "failed";
}

} // namespace

ExtensionsController::ExtensionsController(genesis::project::Editor *editor,
                                           std::filesystem::path store_root, QObject *parent)
    : QObject(parent),
      editor_(editor),
      store_root_(store_root),
      manager_(std::make_shared<genesis::extensions::Manager>(store_root_))
{
    services_.extensions = genesis::api::extensions_seam(manager_);
    genesis::extensions::NativeExtension::Host host;
    host.call_json = [this](const std::string &method, const std::string &params,
                            std::string &out) { return callJson(method, params, out); };
    host.log = extension_log;
    host_ = std::make_unique<genesis::extensions::ExtensionHost>(std::move(store_root),
                                                                 std::move(host));
    refresh();
}

ExtensionsController::~ExtensionsController() = default;

bool ExtensionsController::canUndo() const
{
    return editor_ && editor_->can_undo();
}

bool ExtensionsController::canRedo() const
{
    return editor_ && editor_->can_redo();
}

bool ExtensionsController::dirty() const
{
    return editor_ && editor_->dirty();
}

QString ExtensionsController::readTextFile(const QString &path) const
{
    // A FileDialog hands back a file:// URL; a caller may also pass a plain
    // path. QFile accepts both through QUrl::fromUserInput, which leaves an
    // absolute path alone and decodes a file:// URL.
    const QString local = QUrl(path).isLocalFile() ? QUrl(path).toLocalFile() : path;
    QFile file(local);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

QString ExtensionsController::writeTextFile(const QString &path, const QString &text) const
{
    const QString local = QUrl(path).isLocalFile() ? QUrl(path).toLocalFile() : path;
    // QSaveFile writes to a sibling temp and renames on commit, so a failed
    // export never leaves a half-written profile behind.
    QSaveFile file(local);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return QStringLiteral("cannot write ") + local;
    }
    if (file.write(text.toUtf8()) < 0 || !file.commit()) {
        return QStringLiteral("cannot write ") + local;
    }
    return QString();
}

void ExtensionsController::refresh()
{
    host_->scan();
    pack_roots_ = host_->pack_roots();

    extensions_.clear();
    menus_.clear();
    panels_.clear();

    // The projection lists enabled extensions first, then disabled, then
    // failed. Menus and panels come only from enabled extensions (the others
    // contribute none), so iterating all three is safe.
    const auto project = [this](const genesis::extensions::ExtensionRecord &e) {
        QVariantMap row;
        row[QStringLiteral("id")] = QString::fromStdString(e.id);
        row[QStringLiteral("name")] = QString::fromStdString(e.name);
        row[QStringLiteral("version")] = static_cast<int>(e.version);
        row[QStringLiteral("origin")] = QString::fromStdString(origin_name(e.origin));
        row[QStringLiteral("state")] = QString::fromStdString(state_name(e.state));
        row[QStringLiteral("reason")] = QString::fromStdString(e.reason);
        QStringList deps;
        for (const std::string &required : e.dependencies) {
            deps.append(QString::fromStdString(required));
        }
        row[QStringLiteral("requires")] = deps;
        QStringList dependents;
        for (const std::string &dependent : e.dependents) {
            dependents.append(QString::fromStdString(dependent));
        }
        row[QStringLiteral("dependents")] = dependents;
        row[QStringLiteral("consented")] = e.consented;
        extensions_.append(row);

        for (const genesis::extensions::MenuEntry &menu : e.menus) {
            QVariantMap entry;
            entry[QStringLiteral("id")] = QString::fromStdString(e.id);
            entry[QStringLiteral("path")] = QString::fromStdString(menu.path);
            entry[QStringLiteral("label")] = QString::fromStdString(menu.label);
            entry[QStringLiteral("action")] = QString::fromStdString(menu.action.value_or(""));
            entry[QStringLiteral("call")] = QString::fromStdString(menu.call.value_or(""));
            menus_.append(entry);
        }

        for (const genesis::extensions::PanelEntry &panel : e.panels) {
            QVariantMap entry;
            entry[QStringLiteral("id")] = QString::fromStdString(e.id);
            entry[QStringLiteral("label")] = QString::fromStdString(panel.label);
            entry[QStringLiteral("qml")] = QString::fromStdString(panel.qml);
            entry[QStringLiteral("url")] = QString::fromStdString(panel.url);
            panels_.append(entry);
        }
    };

    for (const genesis::extensions::ExtensionRecord &e : host_->extensions()) {
        project(e);
    }
    for (const genesis::extensions::ExtensionRecord &e : host_->disabled()) {
        project(e);
    }
    for (const genesis::extensions::ExtensionRecord &e : host_->failed()) {
        project(e);
    }

    refusalCount_ = static_cast<int>(host_->refusals().size());

    // The tombstoned builtins, for the Removed section's Restore buttons: every
    // id on the store's removed.json whose recorded origin is Builtin. A
    // removed builtin's origin record persists (removal deletes the installed
    // copy, not the record), so this intersection is the builtins the user
    // removed. `ids` returns a sorted set, so the projection is deterministic.
    removedBuiltins_.clear();
    for (const std::string &id : genesis::extensions::ids(store_root_)) {
        if (host_->origin_of(id) == genesis::extensions::Origin::Builtin) {
            QVariantMap row;
            row[QStringLiteral("id")] = QString::fromStdString(id);
            removedBuiltins_.append(row);
        }
    }

    emit extensionsChanged();
}

void ExtensionsController::grantConsent(const QString &id)
{
    genesis::extensions::Consent consent(store_root_);
    consent.grant(id.toStdString());
    refresh();
}

void ExtensionsController::revokeConsent(const QString &id)
{
    genesis::extensions::Consent consent(store_root_);
    consent.revoke(id.toStdString());
    refresh();
}

void ExtensionsController::setEnabled(const QString &id, bool enabled)
{
    const std::string id_str = id.toStdString();
    if (enabled) {
        manager_->enable(id_str);
    } else {
        manager_->disable(id_str);
    }
    refresh();
}

bool ExtensionsController::removeExtension(const QString &id)
{
    const std::string id_str = id.toStdString();
    bool changed = false;
    if (host_->origin_of(id_str) == genesis::extensions::Origin::Builtin) {
        // Builtins are removed the way the CLI removes them: tombstone first
        // (so the startup seed never re-adds the id), then delete the installed
        // copy, then disable the dependents. The Manager refuses builtins
        // because the API surface treats them as seed-managed; the app's dialog
        // removes them directly.
        if (genesis::extensions::remove(store_root_, id_str)) {
            std::error_code ec;
            std::filesystem::remove_all(store_root_ / id_str, ec);
            manager_->propagate_removed(id_str);
            changed = !ec;
        }
    } else {
        changed = manager_->remove_extension(id_str);
    }
    refresh();
    return changed;
}

bool ExtensionsController::restoreExtension(const QString &id)
{
    const std::string id_str = id.toStdString();
    bool changed = false;
    if (host_->origin_of(id_str) == genesis::extensions::Origin::Builtin) {
        // Restoring a builtin clears its tombstone and re-seeds the builtins,
        // which reinstalls the id (and any other not-tombstoned missing
        // builtin) from the builtin source - the CLI's restore path.
        if (genesis::extensions::restore(store_root_, id_str)) {
            genesis::extensions::seed(store_root_, genesis::extensions::builtin_root());
            changed = true;
        }
    } else {
        changed = manager_->restore_extension(id_str);
    }
    refresh();
    return changed;
}

QString ExtensionsController::install(const QString &sourceDir, const QString &originName)
{
    const std::optional<genesis::extensions::Origin> origin =
            parse_origin(originName.toStdString());
    if (!origin) {
        return QStringLiteral("error: unknown origin '") + originName + "'";
    }

    const genesis::extensions::InstallResult result = genesis::extensions::install_extension(
            std::filesystem::path(sourceDir.toStdString()), store_root_, *origin);
    if (!result.ok()) {
        // The first problem is the actionable one; the rest are the same
        // refusal's detail. A refused install wrote nothing to the store.
        const std::string reason =
                result.problems.empty() ? "install refused" : result.problems.front();
        return QStringLiteral("error: ") + QString::fromStdString(reason);
    }

    // A Developer install lands consent-gated: the re-scan lists it as a
    // failed row whose reason names the missing consent, so the dialog's
    // Consent toggle is the obvious next step. `installed` is
    // `<store>/<id>/<version>`, so its parent names the id and its leaf the
    // version.
    refresh();
    const std::string id = result.installed->parent_path().filename().string();
    const std::string version = result.installed->filename().string();
    return QStringLiteral("installed ") + QString::fromStdString(id) + QStringLiteral(" v")
            + QString::fromStdString(version);
}

void ExtensionsController::setHostServices(genesis::api::Services services)
{
    // The extension-store seam was bound to the store Manager in the
    // constructor; keep it (it holds the Manager) and adopt the app's seams for
    // everything else.
    services.extensions = services_.extensions;
    services_ = std::move(services);
}

QVariant ExtensionsController::invoke(const QString &id, const QString &action, const QString &args)
{
    if (!editor_) {
        return QVariant{ };
    }
    const auto result = host_->invoke(id.toStdString(), action.toStdString(), args.toStdString());
    if (!result) {
        return QVariant{ };
    }
    // A mutating action applies through the one Editor; refresh the shell and
    // history either way (an idempotent reload, so a read-only action costs
    // nothing beyond the refresh it shares with every edit).
    emit historyChanged();
    emit projectEdited();
    return QVariant{ QString::fromStdString(*result) };
}

QString ExtensionsController::call(const QString &method, const QString &paramsJson)
{
    std::string out;
    callJson(method.toStdString(), paramsJson.toStdString(), out);
    return QString::fromStdString(out);
}

int ExtensionsController::callJson(const std::string &method, const std::string &params,
                                   std::string &out)
{
    using nlohmann::json;
    namespace api = genesis::api;

    json params_json = json::object();
    if (!params.empty()) {
        try {
            const json parsed = json::parse(params);
            if (!parsed.is_object()) {
                out = api::wire_error(nullptr, -32600, "params must be a JSON object").dump();
                return 0;
            }
            params_json = parsed;
        } catch (const json::exception &e) {
            out = api::wire_error(nullptr, -32700, e.what()).dump();
            return 0;
        }
    }

    api::DecodeError error;
    const std::optional<api::Request> request = api::decode_request(method, params_json, error);
    if (!request) {
        out = api::wire_error(nullptr, error.code, error.message).dump();
        return 0;
    }
    const api::Response response = api::dispatch(*request, *editor_, services_);
    if (response) {
        out = api::wire_result(nullptr, api::reply_to_json(*response)).dump();
    } else {
        out = api::wire_error(nullptr, api::number(response.error().code), response.error().message)
                      .dump();
    }
    return 0;
}
