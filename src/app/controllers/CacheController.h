// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The studio shell's cache controller: the thin, testable layer that reports
// the processed-media cache's on-disk size and runs the sweep/clear jobs off
// the UI thread (R4). It owns no media and no engine - only the cache root it
// scans. The worker-thread lifecycle and the marshalling of the outcome back to
// the controller's thread live in `OffThreadGenerator`, which this controller
// builds on. The QML pane binds to the properties below and calls the
// Q_INVOKABLEs; the controller never touches QML.
//
// The eviction cap is a plain getter/setter, not a Q_PROPERTY: the persisted
// value lives in AppSettings (`cache.json` in the config dir), and the shell
// pushes it here whenever it changes.

#pragma once

#include <cstdint>
#include <filesystem>

#include "app/controllers/OffThreadGenerator.h"

class CacheController : public OffThreadGenerator
{
    Q_OBJECT

    // The total on-disk size of the cache entries, in bytes. Refreshed by
    // refreshSize/sweep/clearCache (and the startup sweep the shell runs).
    Q_PROPERTY(quint64 sizeBytes READ sizeBytes NOTIFY sizeBytesChanged)
    // True while a sweep/clear/size job runs off the UI thread.
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)

public:
    explicit CacheController(QObject *parent = nullptr);

    quint64 sizeBytes() const { return sizeBytes_; }
    bool running() const { return generating(); }

    // The cache root whose entries the controller manages. Set by the host
    // before the first operation; empty disables every operation (a no-op).
    void setCacheRoot(const std::filesystem::path &root);

    // The eviction cap in bytes (default kDefaultCacheCapBytes). Plain because
    // the persisted value lives in AppSettings and the shell pushes it here.
    quint64 capBytes() const { return capBytes_; }
    void setCapBytes(quint64 bytes);

    // Recomputes sizeBytes without deleting anything.
    Q_INVOKABLE void refreshSize();
    // Evicts the oldest entries until the total fits the cap.
    Q_INVOKABLE void sweep();
    // Deletes every cache entry under the roots.
    Q_INVOKABLE void clearCache();

signals:
    void sizeBytesChanged();
    void runningChanged();

private:
    enum class Operation { Refresh, Sweep, Clear };

    // Dispatches `operation` on a fresh worker thread and posts the result
    // back. No-ops while a run is in flight or the root is empty.
    void run(Operation operation);

    // Runs on the controller's thread, after the base joined the worker and
    // emitted the completion signals: folds a finished run's size in.
    void finish(quint64 new_size);

    std::filesystem::path root_;
    quint64 capBytes_ = 0;
    quint64 sizeBytes_ = 0;
};
