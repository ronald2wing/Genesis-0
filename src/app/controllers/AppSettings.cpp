// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/controllers/AppSettings.h"

#include "app/controllers/MediaBinController.h"
#include "app/controllers/PlaybackController.h"
#include "app/controllers/ProxyController.h"
#include "app/controllers/ScopesController.h"

AppSettings::AppSettings(MediaBinController *bin, PlaybackController *playback,
                         ScopesController *scopes, ProxyController *proxy, QObject *parent)
    : QObject(parent), bin_(bin), playback_(playback), scopes_(scopes), proxy_(proxy)
{
}

QString AppSettings::cacheRoot() const
{
    return bin_->cacheRoot();
}

void AppSettings::setCacheRoot(const QString &value)
{
    if (bin_->cacheRoot() == value) {
        return;
    }
    // Already-generated caches are not migrated; the next cache run (an
    // edit's refresh calls setItems + generateMissingCaches) uses the new
    // root.
    bin_->setCacheRoot(value);
    emit cacheRootChanged();
}

int AppSettings::decodePreference() const
{
    return playback_->decodePreference();
}

void AppSettings::setDecodePreference(int value)
{
    playback_->setDecodePreference(value);
    emit decodePreferenceChanged();
}

int AppSettings::scopesIntervalMs() const
{
    return scopes_->intervalMs();
}

void AppSettings::setScopesIntervalMs(int ms)
{
    scopes_->setIntervalMs(ms);
    emit scopesIntervalMsChanged();
}

bool AppSettings::proxiesEnabled() const
{
    return proxy_->enabled();
}

void AppSettings::setProxiesEnabled(bool enabled)
{
    if (proxy_->enabled() == enabled) {
        return;
    }
    proxy_->setEnabled(enabled);
    emit proxiesEnabledChanged();
}
