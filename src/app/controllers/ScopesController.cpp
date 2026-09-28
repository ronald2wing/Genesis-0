// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/controllers/ScopesController.h"

#include <chrono>
#include <cstddef>
#include <utility>

#include <QMetaObject>
#include <QThread>

namespace engine = genesis::adapters::engine;

namespace {

// The worker's poll interval: ~30 Hz, the budget for a scope pane. The worker
// sleeps this long between samples, so a fresh frame lands at most this late.
constexpr int kMinIntervalMs = 1;

} // namespace

ScopesController::ScopesController(QObject *parent) : QObject(parent)
{
    histogram_.fill(0, static_cast<qsizetype>(engine::kScopeLevels));
    waveformLumaMin_.fill(0, static_cast<qsizetype>(engine::kScopeColumns));
    waveformLumaMax_.fill(0, static_cast<qsizetype>(engine::kScopeColumns));
    waveformRedMin_.fill(0, static_cast<qsizetype>(engine::kScopeColumns));
    waveformRedMax_.fill(0, static_cast<qsizetype>(engine::kScopeColumns));
    waveformGreenMin_.fill(0, static_cast<qsizetype>(engine::kScopeColumns));
    waveformGreenMax_.fill(0, static_cast<qsizetype>(engine::kScopeColumns));
    waveformBlueMin_.fill(0, static_cast<qsizetype>(engine::kScopeColumns));
    waveformBlueMax_.fill(0, static_cast<qsizetype>(engine::kScopeColumns));
    vectorscope_.fill(0, static_cast<qsizetype>(engine::kScopeLevels) * engine::kScopeLevels);
    worker_ = std::thread([this] { workerLoop(); });
}

void ScopesController::stopWorker()
{
    // A controller destroyed while its worker runs must not free members under
    // it: ask it to stop, then join. The worker wakes within one interval, so
    // the join cannot block longer than that. Idempotent: the composition root
    // stops it before owning members it samples teardown, and the destructor
    // calls it again as a fallback.
    shutdown_.store(true, std::memory_order_relaxed);
    if (worker_.joinable()) {
        worker_.join();
    }
}

ScopesController::~ScopesController()
{
    stopWorker();
}

void ScopesController::setEnabled(bool value)
{
    if (enabled_ == value) {
        return;
    }
    enabled_ = value;
    enabledFlag_.store(value, std::memory_order_relaxed);
    if (!value) {
        // Drop any parked frame so a re-enable never publishes a stale one,
        // and report the pane idle now that the scopes are off.
        {
            QMutexLocker lock(&frameMutex_);
            pending_.reset();
        }
        setIdle(true);
    }
    emit enabledChanged();
}

void ScopesController::setMode(int value)
{
    if (mode_ == value) {
        return;
    }
    mode_ = value;
    emit modeChanged();
}

void ScopesController::setFrameSource(FrameSource source)
{
    QMutexLocker lock(&sourceMutex_);
    source_ = std::move(source);
}

void ScopesController::setIntervalMs(int ms)
{
    intervalMs_.store(ms < kMinIntervalMs ? kMinIntervalMs : ms, std::memory_order_relaxed);
}

void ScopesController::workerLoop()
{
    workerThreadId_.store(reinterpret_cast<quintptr>(QThread::currentThread()));
    while (!shutdown_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(
                std::chrono::milliseconds(intervalMs_.load(std::memory_order_relaxed)));
        if (!enabledFlag_.load(std::memory_order_relaxed)) {
            continue;
        }
        // Copy the source under its lock, then sample it without holding it,
        // so setFrameSource never waits on a slow frame.
        FrameSource source;
        {
            QMutexLocker lock(&sourceMutex_);
            source = source_;
        }
        if (!source) {
            continue;
        }
        auto frame = source();
        if (!frame) {
            continue;
        }
        {
            QMutexLocker lock(&frameMutex_);
            pending_ = std::move(frame); // latest wins: overwrite, never queue
        }
        if (!publishQueued_.exchange(true, std::memory_order_acq_rel)) {
            QMetaObject::invokeMethod(this, [this] { publishLatest(); }, Qt::QueuedConnection);
        }
    }
}

void ScopesController::publishLatest()
{
    publishQueued_.store(false, std::memory_order_release);
    std::optional<engine::ScopeFrame> frame;
    {
        QMutexLocker lock(&frameMutex_);
        frame = std::move(pending_);
        pending_.reset();
    }
    if (!frame) {
        return;
    }
    hdr_ = engine::is_hdr_signal(frame->signal);
    scaleMax_ = static_cast<int>(engine::scope_scale_max(frame->signal));
    for (std::size_t i = 0; i < engine::kScopeLevels; ++i) {
        histogram_[i] = static_cast<int>(frame->histogram.luma[i]);
    }
    const auto &wave = frame->waveform;
    for (std::size_t i = 0; i < engine::kScopeColumns; ++i) {
        waveformLumaMin_[i] = static_cast<int>(wave.luma_min[i]);
        waveformLumaMax_[i] = static_cast<int>(wave.luma_max[i]);
        waveformRedMin_[i] = static_cast<int>(wave.red_min[i]);
        waveformRedMax_[i] = static_cast<int>(wave.red_max[i]);
        waveformGreenMin_[i] = static_cast<int>(wave.green_min[i]);
        waveformGreenMax_[i] = static_cast<int>(wave.green_max[i]);
        waveformBlueMin_[i] = static_cast<int>(wave.blue_min[i]);
        waveformBlueMax_[i] = static_cast<int>(wave.blue_max[i]);
    }
    // The vectorscope density is a variable-length vector (empty until a frame
    // has been reduced), unlike the fixed-size histogram and waveform arrays,
    // so copy it only when it carries a full plane.
    const auto &vec = frame->vectorscope;
    if (vec.size == engine::kScopeLevels
        && vec.density.size()
                == static_cast<std::size_t>(engine::kScopeLevels) * engine::kScopeLevels) {
        for (std::size_t i = 0; i < vec.density.size(); ++i) {
            vectorscope_[i] = static_cast<int>(vec.density[i]);
        }
    }
    setIdle(false);
    emit frameChanged();
}

void ScopesController::setIdle(bool value)
{
    if (idle_ == value) {
        return;
    }
    idle_ = value;
    emit idleChanged();
}
