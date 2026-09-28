// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The scopes controller's suite: drives a fake frame source through the
// controller headlessly, proving the off-thread sampling (R4), the latest-wins
// coalescing, the enabled/idle gating, and a clean destruction. No GL context
// and no engine: the fake source returns plain ScopeFrames.

#include <cstdio>
#include <atomic>
#include <memory>
#include <optional>
#include <string>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QObject>
#include <QThread>
#include <QVector>

#include "../support/Wait.h"

#include "adapters/engine/Scopes.h"
#include "app/controllers/ScopesController.h"

using genesis::adapters::engine::ScopeFrame;

namespace {

using genesis::test::wait_quietly;
using genesis::test::wait_until;

int checks = 0;
int failures = 0;

void check(bool condition, const std::string &what)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

// Shared state behind the fake source: how many times it has been sampled and
// how many frames to produce before it goes quiet (returns nullopt). The
// marker (1..limit) is written into luma bin 0, so the published histogram
// reveals which frame the controller kept.
struct FakeState
{
    std::atomic<int> calls{ 0 };
    std::atomic<int> limit{ 0 };
};

ScopesController::FrameSource fake_source(std::shared_ptr<FakeState> state)
{
    return [state]() -> std::optional<ScopeFrame> {
        const int n = state->calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n > state->limit.load(std::memory_order_relaxed)) {
            return std::nullopt;
        }
        ScopeFrame frame;
        frame.histogram.luma[0] = static_cast<std::uint32_t>(n);
        return frame;
    };
}

void test_idle_before_and_after_a_frame()
{
    auto state = std::make_shared<FakeState>();
    state->limit.store(1);
    ScopesController controller;
    controller.setIntervalMs(5);
    controller.setFrameSource(fake_source(state));

    check(controller.idle(), "idle before any frame");

    controller.setEnabled(true);
    check(wait_until([&] { return !controller.idle(); }, 2000), "a frame arrives while enabled");
    check(!controller.idle(), "not idle once a frame arrives");
    const QVector<int> hist = controller.histogram();
    check(hist.size() == 256, "the histogram has 256 bins");
    check(hist[0] == 1, "the frame's marker is published");
}

void test_burst_publishes_latest_coalesced()
{
    constexpr int kBurst = 200;
    auto state = std::make_shared<FakeState>();
    state->limit.store(kBurst);
    ScopesController controller;
    controller.setIntervalMs(1);

    int notifications = 0;
    QObject::connect(&controller, &ScopesController::frameChanged, [&] { ++notifications; });

    controller.setFrameSource(fake_source(state));
    controller.setEnabled(true);

    check(wait_quietly([&] { return state->calls.load() >= kBurst; }, 3000),
          "the worker drains the whole burst");
    check(wait_until([&] { return controller.histogram()[0] == kBurst; }, 2000),
          "the latest frame of the burst is published");

    check(notifications >= 1, "at least one frame is published");
    check(notifications <= 10, "a burst coalesces to a few notifications");
    check(controller.workerThreadId() != reinterpret_cast<quintptr>(QThread::currentThread()),
          "the sampling left the controller's thread");
}

void test_disabled_stops_publishing()
{
    auto state = std::make_shared<FakeState>();
    state->limit.store(1000000); // effectively continuous
    ScopesController controller;
    controller.setIntervalMs(1);

    int notifications = 0;
    QObject::connect(&controller, &ScopesController::frameChanged, [&] { ++notifications; });

    controller.setFrameSource(fake_source(state));
    controller.setEnabled(true);
    check(wait_until([&] { return !controller.idle(); }, 2000), "a frame arrives while enabled");

    controller.setEnabled(false);
    check(controller.idle(), "idle once disabled");

    const int calls_at_disable = state->calls.load();
    const int notes_at_disable = notifications;
    QThread::msleep(60);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

    check(notifications - notes_at_disable <= 1,
          "at most one in-flight publish lands after disable");
    check(state->calls.load() <= calls_at_disable + 2, "the worker stops sampling once disabled");
}

void test_destruction_while_worker_runs()
{
    auto state = std::make_shared<FakeState>();
    state->limit.store(1000000);
    QElapsedTimer timer;
    {
        ScopesController controller;
        controller.setIntervalMs(1);
        controller.setFrameSource(fake_source(state));
        controller.setEnabled(true);
        check(wait_until([&] { return !controller.idle(); }, 2000),
              "a frame arrives before destruction");
        timer.start();
    }
    check(timer.elapsed() < 2000, "destruction joins the worker promptly");
}

void test_waveform_and_vectorscope_publish()
{
    // A source that yields one frame with known waveform/vectorscope values,
    // then goes quiet, so the controller's copies can be asserted exactly.
    auto calls = std::make_shared<std::atomic<int>>(0);
    ScopesController::FrameSource source = [calls]() -> std::optional<ScopeFrame> {
        if (calls->fetch_add(1, std::memory_order_relaxed) != 0) {
            return std::nullopt;
        }
        ScopeFrame frame;
        frame.waveform.columns = 256;
        frame.waveform.luma_min[0] = 10;
        frame.waveform.luma_max[0] = 200;
        frame.waveform.red_max[0] = 255;
        frame.waveform.green_max[1] = 128;
        frame.waveform.blue_min[2] = 32;
        frame.vectorscope.size = 256;
        frame.vectorscope.density.assign(256 * 256, 0);
        frame.vectorscope.density[0] = 7;
        frame.vectorscope.density[256 * 256 - 1] = 9;
        return frame;
    };

    ScopesController controller;
    controller.setIntervalMs(5);
    controller.setFrameSource(std::move(source));
    controller.setEnabled(true);

    check(wait_until([&] { return !controller.idle(); }, 2000),
          "a frame with waveform and vectorscope arrives");

    const QVector<int> lumaMin = controller.waveformLumaMin();
    const QVector<int> lumaMax = controller.waveformLumaMax();
    check(lumaMin.size() == 256 && lumaMax.size() == 256, "the waveform arrays have 256 columns");
    check(lumaMin[0] == 10, "waveform luma min lands in column 0");
    check(lumaMax[0] == 200, "waveform luma max lands in column 0");
    check(controller.waveformRedMax()[0] == 255, "waveform red max lands");
    check(controller.waveformGreenMax()[1] == 128, "waveform green max lands");
    check(controller.waveformBlueMin()[2] == 32, "waveform blue min lands");

    const QVector<int> vec = controller.vectorscope();
    check(vec.size() == 256 * 256, "the vectorscope has 256*256 bins");
    check(vec[0] == 7, "vectorscope bin 0 keeps its density");
    check(vec[256 * 256 - 1] == 9, "the vectorscope's last bin keeps its density");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    test_idle_before_and_after_a_frame();
    test_burst_publishes_latest_coalesced();
    test_disabled_stops_publishing();
    test_destruction_while_worker_runs();
    test_waveform_and_vectorscope_publish();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
