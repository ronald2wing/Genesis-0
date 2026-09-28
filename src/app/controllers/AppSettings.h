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

public:
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

signals:
    void cacheRootChanged();
    void decodePreferenceChanged();
    void scopesIntervalMsChanged();
    void proxiesEnabledChanged();

private:
    MediaBinController *bin_ = nullptr;
    PlaybackController *playback_ = nullptr;
    ScopesController *scopes_ = nullptr;
    ProxyController *proxy_ = nullptr;
};
