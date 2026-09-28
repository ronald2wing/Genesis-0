// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/controllers/AppSettings.h"

#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "app/controllers/MediaBinController.h"
#include "app/controllers/PlaybackController.h"
#include "app/controllers/ProxyController.h"
#include "app/controllers/ScopesController.h"
#include "workspace/CacheSweep.h"

namespace {

// The settings file inside the config dir that carries the persisted catalog
// URL, mirroring UpdateController's updates.json.
constexpr const char *kCatalogFile = "catalog.json";

// The settings file that carries the persisted cache cap, in the same
// config-dir pattern.
constexpr const char *kCacheFile = "cache.json";

// Reads the persisted catalog URL, or nullopt when absent/unreadable/corrupt.
std::optional<std::string> read_catalog_url(const std::filesystem::path &config)
{
    std::ifstream in(config / kCatalogFile);
    if (!in) {
        return std::nullopt;
    }
    const nlohmann::json doc = nlohmann::json::parse(in, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        return std::nullopt;
    }
    const auto it = doc.find("catalogUrl");
    if (it == doc.end() || !it->is_string()) {
        return std::nullopt;
    }
    return it->get<std::string>();
}

// Best-effort write, like UpdateController's feed write: an unwritable config
// dir is not worth failing a setting change over.
void write_catalog_url(const std::filesystem::path &config, std::string_view url)
{
    std::error_code ec;
    std::filesystem::create_directories(config, ec);
    if (ec) {
        return;
    }
    nlohmann::json doc = nlohmann::json::object();
    doc["catalogUrl"] = std::string(url);
    std::ofstream out(config / kCatalogFile);
    if (!out) {
        return;
    }
    out << doc.dump(2) << '\n';
}

// Reads the persisted cache cap, or nullopt when absent/unreadable/corrupt or
// not a non-negative integer.
std::optional<std::uint64_t> read_cache_cap(const std::filesystem::path &config)
{
    std::ifstream in(config / kCacheFile);
    if (!in) {
        return std::nullopt;
    }
    const nlohmann::json doc = nlohmann::json::parse(in, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        return std::nullopt;
    }
    const auto it = doc.find("cacheCapBytes");
    if (it == doc.end() || !it->is_number_unsigned()) {
        return std::nullopt;
    }
    return it->get<std::uint64_t>();
}

// Best-effort write, like the catalog-URL write: an unwritable config dir is
// not worth failing a setting change over.
void write_cache_cap(const std::filesystem::path &config, std::uint64_t bytes)
{
    std::error_code ec;
    std::filesystem::create_directories(config, ec);
    if (ec) {
        return;
    }
    nlohmann::json doc = nlohmann::json::object();
    doc["cacheCapBytes"] = bytes;
    std::ofstream out(config / kCacheFile);
    if (!out) {
        return;
    }
    out << doc.dump(2) << '\n';
}

} // namespace

AppSettings::AppSettings(MediaBinController *bin, PlaybackController *playback,
                         ScopesController *scopes, ProxyController *proxy, QObject *parent)
    : QObject(parent),
      bin_(bin),
      playback_(playback),
      scopes_(scopes),
      proxy_(proxy),
      cacheCapBytes_(genesis::workspace::kDefaultCacheCapBytes)
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

QString AppSettings::catalogUrl() const
{
    return catalogUrl_;
}

void AppSettings::setCatalogUrl(const QString &url)
{
    if (catalogUrl_ == url) {
        return;
    }
    catalogUrl_ = url;
    if (!configDir_.empty()) {
        write_catalog_url(configDir_, url.toStdString());
    }
    emit catalogUrlChanged();
}

quint64 AppSettings::cacheCapBytes() const
{
    return cacheCapBytes_;
}

void AppSettings::setCacheCapBytes(quint64 bytes)
{
    if (cacheCapBytes_ == bytes) {
        return;
    }
    cacheCapBytes_ = bytes;
    if (!configDir_.empty()) {
        write_cache_cap(configDir_, bytes);
    }
    emit cacheCapBytesChanged();
}

void AppSettings::setConfigDir(const std::filesystem::path &dir)
{
    configDir_ = dir;
    if (configDir_.empty()) {
        return;
    }
    if (const std::optional<std::string> saved = read_catalog_url(configDir_)) {
        const QString value = QString::fromStdString(*saved);
        if (catalogUrl_ != value) {
            catalogUrl_ = value;
            emit catalogUrlChanged();
        }
    }
    if (const std::optional<std::uint64_t> saved_cap = read_cache_cap(configDir_)) {
        if (cacheCapBytes_ != *saved_cap) {
            cacheCapBytes_ = *saved_cap;
            emit cacheCapBytesChanged();
        }
    }
}
