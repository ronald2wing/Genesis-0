// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

// The string catalogue's suite: locale detection, English fallback, missing-key
// passthrough, and the shipped pseudo-locale's parse/apply — all headless. The
// shipped-asset cases run with the source dir as cwd so AssetPaths::asset_root()
// walks up to assets/i18n/.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "../support/Checks.h"

#include "app/support/I18n.h"
#include "workspace/AssetPaths.h"

namespace {

using genesis::test::check;
using genesis::test::summary;

void writeFile(const QString &path, const char *contents)
{
    std::ofstream out(path.toStdString(), std::ios::binary);
    out << contents;
}

// The string-valued keys of a catalogue file, as a sorted set. `_`-prefixed
// metadata keys (`_meta`) are objects, never strings, so they are excluded here
// and the set holds exactly the translatable keys.
std::set<std::string> stringKeys(const QString &path)
{
    std::set<std::string> keys;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return keys;
    }
    QJsonParseError error{ };
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return keys;
    }
    const QJsonObject object = document.object();
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (it.value().isString()) {
            keys.insert(it.key().toStdString());
        }
    }
    return keys;
}

// The `_meta` value for a catalogue file, empty when absent or not an object.
QJsonObject metaObject(const QString &path)
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
    const QJsonObject object = document.object();
    const QJsonValue meta = object.value(QStringLiteral("_meta"));
    return meta.isObject() ? meta.toObject() : QJsonObject{ };
}

// A temp locale directory with an English base and a partial German overlay,
// so the fallback rules are exercised against a known catalogue.
struct LocaleFixture
{
    QTemporaryDir temp;
    QString dir;

    LocaleFixture()
    {
        dir = temp.path();
        writeFile(dir + QStringLiteral("/en.json"),
                  "{\"app.title\":\"English\",\"common.onlyEnglish\":\"Only in English\"}");
        writeFile(dir + QStringLiteral("/de.json"), "{\"app.title\":\"Deutsch\"}");
    }
};

void test_english_base()
{
    LocaleFixture fixture;
    I18n i18n(fixture.dir, QStringLiteral("en"));
    check(i18n.currentLocale() == QStringLiteral("en"), "english locale selected");
    check(i18n.translate(QStringLiteral("app.title")) == QStringLiteral("English"),
          "english key resolves");
    check(i18n.translate(QStringLiteral("common.onlyEnglish")) == QStringLiteral("Only in English"),
          "english-only key resolves");
}

void test_locale_overlay_and_fallback()
{
    LocaleFixture fixture;
    I18n i18n(fixture.dir, QStringLiteral("de"));
    check(i18n.translate(QStringLiteral("app.title")) == QStringLiteral("Deutsch"),
          "locale overrides the english value");
    check(i18n.translate(QStringLiteral("common.onlyEnglish")) == QStringLiteral("Only in English"),
          "a key absent from the locale falls back to english");
}

void test_missing_key_returns_the_key()
{
    LocaleFixture fixture;
    I18n i18n(fixture.dir, QStringLiteral("de"));
    check(i18n.translate(QStringLiteral("does.not.exist")) == QStringLiteral("does.not.exist"),
          "an unknown key resolves to itself");
}

void test_missing_locale_falls_back_to_english()
{
    LocaleFixture fixture;
    I18n i18n(fixture.dir, QStringLiteral("fr"));
    check(i18n.currentLocale() == QStringLiteral("en"), "a missing locale falls back to english");
    check(i18n.translate(QStringLiteral("app.title")) == QStringLiteral("English"),
          "the fallback catalogue is english");
}

void test_language_only_fallback()
{
    LocaleFixture fixture;
    // "de_DE" is requested but only "de" ships, so the language-level file wins.
    I18n i18n(fixture.dir, QStringLiteral("de_DE"));
    check(i18n.currentLocale() == QStringLiteral("de"), "language-only file matched");
    check(i18n.translate(QStringLiteral("app.title")) == QStringLiteral("Deutsch"),
          "language-only catalogue resolves");
}

void test_environment_override()
{
    LocaleFixture fixture;
    qputenv("GENESIS_LOCALE", "de");
    I18n i18n(fixture.dir, QString());
    const QString selected = i18n.currentLocale();
    qunsetenv("GENESIS_LOCALE");
    check(selected == QStringLiteral("de"), "GENESIS_LOCALE selects the locale");
}

void test_available_locales_listing()
{
    LocaleFixture fixture;
    I18n i18n(fixture.dir, QStringLiteral("en"));
    const QStringList locales = i18n.availableLocales();
    check(locales.size() == 2, "both locales are listed");
    check(locales.at(0) == QStringLiteral("de") && locales.at(1) == QStringLiteral("en"),
          "available locales are listed, sorted");
}

void test_shipped_english()
{
    const QString dir =
            QString::fromStdString((genesis::workspace::asset_root() / "i18n").string());
    I18n i18n(dir, QStringLiteral("en"));
    check(i18n.translate(QStringLiteral("start.open")) == QStringLiteral("Open"),
          "shipped english catalogue resolves");
    check(i18n.translate(QStringLiteral("app.title")) == QStringLiteral("Genesis-0"),
          "shipped product title resolves");
    check(i18n.availableLocales().contains(QStringLiteral("en-XA")), "the pseudo-locale is listed");
}

void test_pseudo_locale_applies()
{
    const QString dir =
            QString::fromStdString((genesis::workspace::asset_root() / "i18n").string());
    I18n en(dir, QStringLiteral("en"));
    I18n xa(dir, QStringLiteral("en-XA"));
    check(xa.currentLocale() == QStringLiteral("en-XA"), "pseudo-locale selected");

    const QString open = xa.translate(QStringLiteral("start.open"));
    check(open != en.translate(QStringLiteral("start.open")), "pseudo text differs from english");
    check(open.startsWith(QLatin1Char('[')) && open.contains(QLatin1Char(']')),
          "pseudo text is wrapped in markers");
    // Accented vowels widen the UTF-8 encoding beyond the character count,
    // proving the transform, and the padding expands the string.
    check(open.toUtf8().size() > open.size(), "pseudo text contains non-ASCII accents");
    check(open.size() > en.translate(QStringLiteral("start.open")).size(),
          "pseudo text is elongated");
}

void test_shipped_locale_completeness()
{
    const QString dir =
            QString::fromStdString((genesis::workspace::asset_root() / "i18n").string());

    // The English catalogue is the source of truth: every shipped locale must
    // carry exactly these translatable keys — no missing, no extra.
    const std::set<std::string> english = stringKeys(dir + QStringLiteral("/en.json"));
    check(english.size() == 12, "english catalogue has 12 keys");

    // 14 languages (en plus 13 machine-drafted translations) and the
    // pseudo-locale, all of which ship and must parse.
    const std::vector<std::string> shipped = { "de", "es",      "fa",    "fr",    "hr",
                                               "it", "ja",      "ko",    "pt-BR", "ru",
                                               "tr", "zh-Hans", "zh-TW", "en-XA" };
    const std::vector<std::string> drafts = { "de", "es",    "fa", "fr", "hr",      "it",   "ja",
                                              "ko", "pt-BR", "ru", "tr", "zh-Hans", "zh-TW" };

    for (const std::string &locale : shipped) {
        const QString name = QString::fromStdString(locale);
        I18n i18n(dir, name);
        check(i18n.currentLocale() == name, locale + " catalogue parses and is selected");
        check(stringKeys(dir + QStringLiteral("/") + name + QStringLiteral(".json")) == english,
              locale + " covers exactly the english keys");
        // Every English key resolves to a real value (the locale's own or the
        // English fallback), never back to the key itself.
        bool covered = true;
        for (const std::string &key : english) {
            if (i18n.translate(QString::fromStdString(key)) == QString::fromStdString(key)) {
                covered = false;
            }
        }
        check(covered, locale + " resolves every english key");
    }

    for (const std::string &locale : drafts) {
        const QJsonObject meta =
                metaObject(dir + QStringLiteral("/") + QString::fromStdString(locale)
                           + QStringLiteral(".json"));
        check(meta.value(QStringLiteral("status")).toString()
                      == QStringLiteral("unreviewed-machine-draft"),
              locale + " is marked an unreviewed machine draft");
        check(meta.value(QStringLiteral("source")).toString() == QStringLiteral("en"),
              locale + " records english as its source");
    }

    // The English source and the pseudo-locale carry no review metadata.
    check(metaObject(dir + QStringLiteral("/en.json")).isEmpty(), "english has no _meta");
    check(metaObject(dir + QStringLiteral("/en-XA.json")).isEmpty(), "pseudo-locale has no _meta");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    test_english_base();
    test_locale_overlay_and_fallback();
    test_missing_key_returns_the_key();
    test_missing_locale_falls_back_to_english();
    test_language_only_fallback();
    test_environment_override();
    test_available_locales_listing();
    test_shipped_english();
    test_pseudo_locale_applies();
    test_shipped_locale_completeness();

    return summary();
}
