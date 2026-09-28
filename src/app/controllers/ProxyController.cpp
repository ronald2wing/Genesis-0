// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/controllers/ProxyController.h"

#include <utility>
#include <vector>

#include "adapters/engine/ges/media/Proxy.h"
#include "core/MediaCache.h"
#include "render/ProxyCache.h"
#include "project/Project.h"
#include "project/model/Media.h"

namespace ges = genesis::adapters::engine::ges;
using genesis::project::MediaKind;
using genesis::project::Project;

ProxyController::ProxyController(QObject *parent) : OffThreadGenerator(parent) { }

void ProxyController::setEnabled(bool value)
{
    if (enabled_ == value) {
        return;
    }
    enabled_ = value;
    emit enabledChanged();
}

void ProxyController::generateMissingProxies(const Project &project,
                                             const std::filesystem::path &cache_root)
{
    if (generating()) {
        return;
    }
    if (!enabled_ || cache_root.empty()) {
        return;
    }

    // Snapshot the video sources by value, so the worker never reads the
    // project, which a concurrent edit could mutate. Only footage decodes a
    // costly stream; a still or a sound-only file has no proxy to make.
    std::vector<std::filesystem::path> sources;
    for (const genesis::project::MediaItem &media : project.media) {
        if (media.kind == MediaKind::Video && !media.path.empty()) {
            sources.emplace_back(media.path);
        }
    }
    if (sources.empty()) {
        return;
    }

    start([this, sources = std::move(sources), cache_root]() {
        bool generated = false;
        for (const std::filesystem::path &source : sources) {
            if (cancelRequested()) {
                break;
            }
            // A fresh proxy is not a generation: the shell reloads the preview
            // only when this run wrote a file it would now read.
            const std::uintmax_t source_size = genesis::core::cache_source_size(source);
            if (source_size != 0
                && genesis::core::cache_is_fresh(
                        genesis::render::proxy_cache_path(cache_root, source),
                        genesis::render::proxy_key(source, source_size))) {
                continue;
            }
            if (ges::ensure_proxy(source, cache_root)) {
                generated = true;
            }
        }
        return generated;
    });
}
