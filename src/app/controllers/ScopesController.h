// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The studio shell's scopes controller: the thin, testable layer that samples
// the engine's scope frames off the UI thread and publishes the latest one to
// the scopes pane. It owns no media and no GL context - only the frame source
// it polls, the worker thread that does the polling (R4), and the marshalling
// of the latest frame back to the controller's thread with latest-wins
// coalescing. The QML pane binds to the properties below and calls the
// setters; the controller itself never touches QML.
//
// The frame source is injected (setFrameSource) so the controller is testable
// without a GL context, and so the adapter stays a detail. The shell wires the
// adapter's Scopes::latest() as the source:
//   controller.setFrameSource([&scopes] { return scopes.latest(); });
// Until then the source is empty, which reads as "no scopes" and stays idle.

#pragma once

#include <QObject>
#include <QMutex>
#include <QVector>

#include <atomic>
#include <functional>
#include <optional>
#include <thread>

#include "adapters/engine/ScopeTypes.h"

class ScopesController : public QObject
{
    Q_OBJECT

    // True while the pane should sample and publish; false parks nothing.
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    // Which scope the pane draws: Histogram (0), Waveform (1), Vectorscope (2),
    // Parade (3).
    Q_PROPERTY(int mode READ mode WRITE setMode NOTIFY modeChanged)
    // True before any frame has arrived and while the scopes are off.
    Q_PROPERTY(bool idle READ idle NOTIFY idleChanged)
    // The Rec.709 luma histogram, 256 bins.
    Q_PROPERTY(QVector<int> histogram READ histogram NOTIFY frameChanged)
    // The waveform scope: per-column min/max display value (0..255) for each
    // channel, 256 columns. The pane draws a vertical span per column; an
    // untouched column keeps the empty sentinel (min 255, max 0).
    Q_PROPERTY(QVector<int> waveformLumaMin READ waveformLumaMin NOTIFY frameChanged)
    Q_PROPERTY(QVector<int> waveformLumaMax READ waveformLumaMax NOTIFY frameChanged)
    Q_PROPERTY(QVector<int> waveformRedMin READ waveformRedMin NOTIFY frameChanged)
    Q_PROPERTY(QVector<int> waveformRedMax READ waveformRedMax NOTIFY frameChanged)
    Q_PROPERTY(QVector<int> waveformGreenMin READ waveformGreenMin NOTIFY frameChanged)
    Q_PROPERTY(QVector<int> waveformGreenMax READ waveformGreenMax NOTIFY frameChanged)
    Q_PROPERTY(QVector<int> waveformBlueMin READ waveformBlueMin NOTIFY frameChanged)
    Q_PROPERTY(QVector<int> waveformBlueMax READ waveformBlueMax NOTIFY frameChanged)
    // The vectorscope: a 256x256 Cb/Cr density grid, row-major, row 0 at the
    // top (+Cr). Each entry counts the pixels that landed in that plane bin.
    Q_PROPERTY(QVector<int> vectorscope READ vectorscope NOTIFY frameChanged)
    // Whether the latest frame's bins read on a perceptual (HDR) nit scale
    // rather than raw display code values.
    Q_PROPERTY(bool hdr READ hdr NOTIFY frameChanged)
    // What scope bin 255 represents for the latest frame: 255 for SDR, 1000 for
    // HLG, 10000 for PQ. The HDR scale maximum in nits; the panel labels its
    // scale with it.
    Q_PROPERTY(int scaleMax READ scaleMax NOTIFY frameChanged)

public:
    // Where scope frames come from. Injected so the controller is testable
    // without a GL context, and so the adapter stays a detail.
    using FrameSource = std::function<std::optional<genesis::adapters::engine::ScopeFrame>()>;

    // The scope pane's display mode.
    enum Mode {
        Histogram = 0,
        Waveform = 1,
        Vectorscope = 2,
        Parade = 3,
    };
    Q_ENUM(Mode)

    explicit ScopesController(QObject *parent = nullptr);
    ~ScopesController() override;

    // Stops and joins the sampling worker. Idempotent; called by the composition
    // root before it frees the frame source the worker polls, and again by the
    // destructor. After this returns the worker thread is gone.
    void stopWorker();

    bool enabled() const { return enabled_; }
    int mode() const { return mode_; }
    bool idle() const { return idle_; }
    QVector<int> histogram() const { return histogram_; }
    QVector<int> waveformLumaMin() const { return waveformLumaMin_; }
    QVector<int> waveformLumaMax() const { return waveformLumaMax_; }
    QVector<int> waveformRedMin() const { return waveformRedMin_; }
    QVector<int> waveformRedMax() const { return waveformRedMax_; }
    QVector<int> waveformGreenMin() const { return waveformGreenMin_; }
    QVector<int> waveformGreenMax() const { return waveformGreenMax_; }
    QVector<int> waveformBlueMin() const { return waveformBlueMin_; }
    QVector<int> waveformBlueMax() const { return waveformBlueMax_; }
    QVector<int> vectorscope() const { return vectorscope_; }
    bool hdr() const { return hdr_; }
    int scaleMax() const { return scaleMax_; }

    void setEnabled(bool value);
    void setMode(int value);

    // Injects the frame source the worker polls. May be called before or while
    // enabled; the swap is serialised against the worker with a mutex.
    void setFrameSource(FrameSource source);

    // The worker's poll interval in milliseconds; floored at 1. Default 33
    // (~30 Hz), the budget for a scope pane. Takes effect on the next tick.
    void setIntervalMs(int ms);
    int intervalMs() const { return intervalMs_.load(); }

    // The worker thread id, or 0 before the worker starts. For the tests to
    // prove the sampling left the controller's thread (R4).
    quintptr workerThreadId() const { return workerThreadId_.load(); }

signals:
    void enabledChanged();
    void modeChanged();
    void idleChanged();
    void frameChanged();

private:
    // Runs on the worker thread for the controller's lifetime: sleeps the
    // interval, samples the source when enabled, and parks the latest frame.
    void workerLoop();

    // Runs on the controller's thread: publishes the latest parked frame.
    void publishLatest();

    void setIdle(bool value);

    FrameSource source_; // guarded by sourceMutex_

    bool enabled_ = false;
    int mode_ = Mode::Histogram;
    bool idle_ = true;
    QVector<int> histogram_; // controller-thread only
    QVector<int> waveformLumaMin_;
    QVector<int> waveformLumaMax_;
    QVector<int> waveformRedMin_;
    QVector<int> waveformRedMax_;
    QVector<int> waveformGreenMin_;
    QVector<int> waveformGreenMax_;
    QVector<int> waveformBlueMin_;
    QVector<int> waveformBlueMax_;
    QVector<int> vectorscope_;
    bool hdr_ = false;
    int scaleMax_ = 255;

    QMutex sourceMutex_;
    QMutex frameMutex_;
    std::optional<genesis::adapters::engine::ScopeFrame> pending_;

    std::atomic<bool> enabledFlag_{ false };
    std::atomic<bool> shutdown_{ false };
    std::atomic<bool> publishQueued_{ false };
    std::atomic<int> intervalMs_{ 33 };
    std::atomic<quintptr> workerThreadId_{ 0 };

    std::thread worker_;
};
