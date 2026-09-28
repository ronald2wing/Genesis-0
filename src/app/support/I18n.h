// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The string catalogue behind `i18n.translate(key)`, so UI strings live in one
// place and can be translated without a Qt translation file. Catalogues are
// JSON files under assets/i18n/ named `<locale>.json` — a flat key -> string
// map — loaded at construction (see docs/building.md, "Translations").
// Top-level keys beginning with '_' (e.g. `_meta`) are metadata and ignored
// by the loader. The
// method is named `translate`, not `tr`, so it does not shadow QObject::tr.
//
// The active locale resolves in this order: the explicit `localeName` argument,
// then the GENESIS_LOCALE environment variable, then QLocale::system().name().
// A key missing from the active locale falls back to the English catalogue, and
// a key missing from English falls back to the key itself, so an untranslated
// or missing string is visible rather than fatal.
//
// The QML context property remains named "i18n" (set in AppShell.cpp); only
// this C++ type carries the name.

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <map>
#include <string>
#include <vector>

class I18n : public QObject
{
    Q_OBJECT
public:
    // Builds the catalogue from the runtime's assets/i18n/ directory.
    explicit I18n(QObject *parent = nullptr);

    // Builds the catalogue from `localeDir` (for tests and headless callers).
    // `localeName` names the locale to load; an empty value defers to the
    // environment/QLocale detection.
    I18n(const QString &localeDir, const QString &localeName, QObject *parent = nullptr);

    // The translated string for `key`: the active locale's value when present,
    // else the English value, else `key` itself.
    Q_INVOKABLE QString translate(const QString &key) const;

    // The locale names discovered in the directory (filename minus ".json"),
    // sorted. Includes the pseudo-locale "en-XA" when its file ships.
    Q_INVOKABLE QStringList availableLocales() const;

    // The locale actually selected after fallback, e.g. "en" or "en-XA".
    Q_INVOKABLE QString currentLocale() const;

private:
    std::map<std::string, std::string> catalogue_;
    std::vector<std::string> locales_;
    QString currentLocale_;
};
