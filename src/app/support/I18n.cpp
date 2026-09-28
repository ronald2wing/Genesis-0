// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/support/I18n.h"

#include "workspace/AssetPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>

#include <algorithm>
#include <cctype>

namespace {

// A locale name folded to one spelling so BCP-47 ("en-US") and QLocale
// ("en_US") forms match the same file: lowercase and '-' -> '_'.
std::string fold(const std::string &name)
{
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        if (c == '-') {
            out.push_back('_');
        } else {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

// Resolves `requested` against the raw available names, or returns "en" when
// nothing matches (a missing locale degrades to English, never to failure).
std::string matchLocale(const std::string &requested, const std::vector<std::string> &available)
{
    if (requested.empty()) {
        return "en";
    }
    const std::string want = fold(requested);
    for (const std::string &name : available) {
        if (fold(name) == want) {
            return name;
        }
    }
    // "en_US" -> "en" when only the language-level file ships.
    const std::size_t sep = want.find('_');
    if (sep != std::string::npos) {
        const std::string language = want.substr(0, sep);
        for (const std::string &name : available) {
            if (fold(name) == language) {
                return name;
            }
        }
    }
    return "en";
}

// The key -> string map from a locale JSON file, empty on any failure. A
// malformed or missing catalogue must not abort startup — it only falls
// through to English.
std::map<std::string, std::string> loadCatalogue(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return { };
    }
    QJsonParseError error{ };
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return { };
    }
    std::map<std::string, std::string> out;
    const QJsonObject object = document.object();
    for (auto it = object.begin(); it != object.end(); ++it) {
        // Top-level keys beginning with '_' (`_meta`, …) are loader-private
        // metadata and never part of the translatable catalogue.
        if (it.key().startsWith(QLatin1Char('_'))) {
            continue;
        }
        if (it.value().isString()) {
            out[it.key().toStdString()] = it.value().toString().toStdString();
        }
    }
    return out;
}

// Sorted locale names (filename minus ".json") found under `dir`.
std::vector<std::string> listLocales(const QString &dir)
{
    const QStringList entries = QDir(dir).entryList({ QStringLiteral("*.json") }, QDir::Files);
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(entries.size()));
    for (const QString &entry : entries) {
        out.push_back(QFileInfo(entry).completeBaseName().toStdString());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The locale the environment requests: GENESIS_LOCALE first, else the system
// locale Qt reports.
QString detectedLocale()
{
    const QByteArray env = qgetenv("GENESIS_LOCALE");
    if (!env.isEmpty()) {
        return QString::fromUtf8(env).trimmed();
    }
    return QLocale::system().name();
}

} // namespace

I18n::I18n(QObject *parent) : I18n(QString(), QString(), parent) { }

I18n::I18n(const QString &localeDir, const QString &localeName, QObject *parent) : QObject(parent)
{
    QString dir = localeDir;
    if (dir.isEmpty()) {
        dir = QString::fromStdString((genesis::workspace::asset_root() / "i18n").string());
    }

    locales_ = listLocales(dir);
    catalogue_ = loadCatalogue(dir + QStringLiteral("/en.json"));

    const QString requested = localeName.isEmpty() ? detectedLocale() : localeName;
    currentLocale_ = QString::fromStdString(matchLocale(requested.toStdString(), locales_));

    if (currentLocale_ != QStringLiteral("en")) {
        const auto overlay =
                loadCatalogue(dir + QStringLiteral("/") + currentLocale_ + QStringLiteral(".json"));
        for (const auto &[key, value] : overlay) {
            catalogue_[key] = value;
        }
    }
}

QString I18n::translate(const QString &key) const
{
    const auto it = catalogue_.find(key.toStdString());
    if (it == catalogue_.end()) {
        return key;
    }
    return QString::fromStdString(it->second);
}

QStringList I18n::availableLocales() const
{
    QStringList out;
    out.reserve(static_cast<int>(locales_.size()));
    for (const std::string &name : locales_) {
        out.append(QString::fromStdString(name));
    }
    return out;
}

QString I18n::currentLocale() const
{
    return currentLocale_;
}
