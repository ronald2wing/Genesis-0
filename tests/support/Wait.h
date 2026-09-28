// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Shared test waiters: bounded predicate polls that keep the Qt event loop
// alive (or deliberately quiet) while a worker settles. The controller suites
// each previously carried their own copy of these; a single header keeps the
// pump/backoff/final-drain contract in one place.

#pragma once

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>

namespace genesis::test {

// Pumps the Qt event loop while polling `pred`, returning whether `pred` held
// within `timeout_ms`. The trailing pump drains queued events that flip the
// predicate just before the deadline, so callers observe the settled state.
template <typename Pred>
bool wait_until(Pred pred, int timeout_ms = 30000)
{
    QElapsedTimer timer;
    timer.start();
    while (!pred() && timer.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return pred();
}

// Idle-waiter: waits until `controller`'s run ends. `idle` is a callable that
// reports the controller's own settled state - e.g.
// `[&]{ return !controller.running(); }` or `[&]{ return !controller.generating(); }`
// - so one helper serves every controller's running()/busy/worker-join
// contract, whatever its method is named.
template <typename Controller, typename Idle>
bool wait_until_idle(Controller &controller, Idle idle, int timeout_ms = 30000)
{
    (void)controller; // the idle callable captures the state under test
    return wait_until(idle, timeout_ms);
}

// Bounded poll that does NOT pump the event loop, for callers that must let
// worker-side state pile up while the UI thread stays quiet (a coalescing test
// asserts a bounded notification count and would race if the loop pumped here).
template <typename Pred>
bool wait_quietly(Pred pred, int timeout_ms = 30000)
{
    QElapsedTimer timer;
    timer.start();
    while (!pred() && timer.elapsed() < timeout_ms) {
        QThread::msleep(2);
    }
    return pred();
}

} // namespace genesis::test
