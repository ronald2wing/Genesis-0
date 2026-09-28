// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/controllers/CacheController.h"

#include <utility>
#include <vector>

#include "workspace/CacheSweep.h"

namespace workspace = genesis::workspace;

CacheController::CacheController(QObject *parent)
    : OffThreadGenerator(parent), capBytes_(workspace::kDefaultCacheCapBytes)
{
    // `running` mirrors the base's `generating` state; forward the one signal
    // so the pane's binding refreshes with it.
    connect(this, &OffThreadGenerator::generatingChanged, this, &CacheController::runningChanged);
}

void CacheController::setCacheRoot(const std::filesystem::path &root)
{
    root_ = root;
}

void CacheController::setCapBytes(quint64 bytes)
{
    capBytes_ = bytes;
}

void CacheController::refreshSize()
{
    run(Operation::Refresh);
}

void CacheController::sweep()
{
    run(Operation::Sweep);
}

void CacheController::clearCache()
{
    run(Operation::Clear);
}

void CacheController::run(Operation operation)
{
    if (generating() || root_.empty()) {
        return;
    }
    const std::vector<std::filesystem::path> roots = workspace::cache_roots(root_);
    const quint64 cap = capBytes_;
    start(
            [operation, roots = std::move(roots), cap]() -> quint64 {
                switch (operation) {
                case Operation::Refresh:
                    return workspace::cache_size_bytes(workspace::cache_entries(roots));
                case Operation::Sweep:
                    return workspace::sweep(roots, cap).bytes_remaining;
                case Operation::Clear:
                    workspace::clear_cache(roots);
                    return 0;
                }
                return 0;
            },
            [this](quint64 size) { finish(size); });
}

void CacheController::finish(quint64 new_size)
{
    // Runs on the controller's thread, after the base joined the worker and
    // emitted the completion signals. Fold the outcome in.
    if (sizeBytes_ != new_size) {
        sizeBytes_ = new_size;
        emit sizeBytesChanged();
    }
}
