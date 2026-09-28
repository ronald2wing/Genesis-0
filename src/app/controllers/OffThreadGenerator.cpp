// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/controllers/OffThreadGenerator.h"

#include <QMetaObject>
#include <QThread>

#include <utility>

OffThreadGenerator::OffThreadGenerator(QObject *parent) : QObject(parent) { }

OffThreadGenerator::~OffThreadGenerator()
{
    // A controller destroyed mid-run must not free its members under the
    // worker: ask it to stop, then join. The work is bounded by the subclass's
    // own guard, so the join cannot block forever.
    cancelRequested_.store(true);
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

bool OffThreadGenerator::start(std::function<void()> work)
{
    if (generating_) {
        return false;
    }
    cancelRequested_.store(false);
    workerThreadId_.store(0);
    generating_ = true;
    emit generatingChanged();

    workerThread_ = std::thread([this, work = std::move(work)]() mutable {
        workerThreadId_.store(reinterpret_cast<quintptr>(QThread::currentThread()));
        work();
        QMetaObject::invokeMethod(this, [this]() { finishGeneration(); }, Qt::QueuedConnection);
    });
    return true;
}

void OffThreadGenerator::finishGeneration()
{
    // Runs on the controller's thread. Join the worker - which has already
    // posted this call and is returning - then emit the completion signals the
    // shell uses to reload the engine timeline with the fresh caches.
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    generating_ = false;
    emit generatingChanged();
    emit finished();
}
