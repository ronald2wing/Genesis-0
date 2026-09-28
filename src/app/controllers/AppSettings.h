// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The settings dialog's facade: one object the QML dialog binds to, forwarding
// each field to the controller or adapter that owns it. scopesIntervalMs is
// here because ScopesController does not publish it as a Q_PROPERTY, only as a
// plain getter/setter.

#pragma once

#include <QObject>
#include <QString>

#include <filesystem>
#include <string_view>

class MediaBinController;
class PlaybackController;
class ProxyController;
class ScopesController;

class AppSettings : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString cacheRoot READ cacheRoot WRITE setCacheRoot NOTIFY cacheRootChanged)
    Q_PROPERTY(int decodePreference READ decodePreference WRITE setDecodePreference NOTIFY
                       decodePreferenceChanged)
    Q_PROPERTY(int scopesIntervalMs READ scopesIntervalMs WRITE setScopesIntervalMs NOTIFY
                       scopesIntervalMsChanged)
    Q_PROPERTY(bool proxiesEnabled READ proxiesEnabled WRITE setProxiesEnabled NOTIFY
                       proxiesEnabledChanged)
    // The marketplace catalog URL, persisted per the UpdateController pattern
    // (a JSON file in the config dir). `catalogUrl()` returns the persisted
    // value, which falls back to kDefaultCatalogUrl when nothing is saved.
    Q_PROPERTY(QString catalogUrl READ catalogUrl WRITE setCatalogUrl NOTIFY catalogUrlChanged)
    // The processed-media cache cap in bytes, persisted the same way (a JSON
    // file in the config dir). `cacheCapBytes()` returns the persisted value,
    // which falls back to genesis::workspace::kDefaultCacheCapBytes when
    // nothing is saved. The shell pushes it into the cache controller.
    Q_PROPERTY(quint64 cacheCapBytes READ cacheCapBytes WRITE setCacheCapBytes NOTIFY
                       cacheCapBytesChanged)

public:
    // The default catalog URL when nothing has been persisted. Empty: no hosted
    // feed is published yet, so the marketplace starts blank and the user pastes
    // a URL (or a `file://` path for a local catalog). Set this to the hosted
    // feed URL when one is published.
    static inline constexpr std::string_view kDefaultCatalogUrl = "";

    AppSettings(MediaBinController *bin, PlaybackController *playback, ScopesController *scopes,
                ProxyController *proxy, QObject *parent = nullptr);

    QString cacheRoot() const;
    void setCacheRoot(const QString &value);

    int decodePreference() const;
    void setDecodePreference(int value);

    int scopesIntervalMs() const;
    void setScopesIntervalMs(int ms);

    bool proxiesEnabled() const;
    void setProxiesEnabled(bool enabled);

    QString catalogUrl() const;
    void setCatalogUrl(const QString &url);

    quint64 cacheCapBytes() const;
    void setCacheCapBytes(quint64 bytes);

    // Where the catalog URL persists (`<dir>/catalog.json`), the same
    // config-dir pattern UpdateController uses. Empty disables persistence (a
    // headless test that does not care); setting it loads any saved URL.
    void setConfigDir(const std::filesystem::path &dir);

signals:
    void cacheRootChanged();
    void decodePreferenceChanged();
    void scopesIntervalMsChanged();
    void proxiesEnabledChanged();
    void catalogUrlChanged();
    void cacheCapBytesChanged();

private:
    MediaBinController *bin_ = nullptr;
    PlaybackController *playback_ = nullptr;
    ScopesController *scopes_ = nullptr;
    ProxyController *proxy_ = nullptr;

    QString catalogUrl_;
    quint64 cacheCapBytes_ = 0;
    std::filesystem::path configDir_;
};
