// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The studio shell's proxy controller: the thin, off-thread generation of the
// low-resolution stand-ins a preview's clips read through. It owns no media
// and no engine session - only the enabled toggle and the worker thread that
// fills the proxy cache (R4, shared with the reverse controller through
// `OffThreadGenerator`). The shell reloads the engine timeline when `finished`
// fires, so a generated proxy lands in the preview on the next build; export
// never consults this controller and always reads originals.

#pragma once

#include <filesystem>

#include "app/controllers/OffThreadGenerator.h"

namespace genesis::project {
struct Project;
}

class ProxyController : public OffThreadGenerator
{
    Q_OBJECT

    // The settings toggle: when off (the default, matching the pre-proxy
    // behaviour) the shell neither generates nor reads proxies.
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)

public:
    explicit ProxyController(QObject *parent = nullptr);

    bool enabled() const { return enabled_; }
    void setEnabled(bool value);

    // Generates a proxy for every video media item off the UI thread. No-ops
    // while already generating, when proxies are disabled, or when the cache
    // root is empty. Emits `finished` on the controller's thread when the run
    // ends, whatever it produced.
    void generateMissingProxies(const genesis::project::Project &project,
                                const std::filesystem::path &cache_root);

signals:
    void enabledChanged();

private:
    bool enabled_ = false;
};
