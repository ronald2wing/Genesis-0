// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The shared base for the studio shell's off-thread cache-fill controllers
// (reverse, proxy, cache): it owns the worker thread's lifecycle, the running
// flag, and the marshalling of completion back to the controller's thread. A
// subclass supplies the work that runs off the UI thread and keeps its own
// public surface - the `enabled` toggle, the generate* entry point, and the
// `finished`/`generatingChanged` signals the shell wires to a timeline reload.

#pragma once

#include <QObject>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

class OffThreadGenerator : public QObject
{
    Q_OBJECT

    // True while a worker fills the cache.
    Q_PROPERTY(bool generating READ generating NOTIFY generatingChanged)

public:
    explicit OffThreadGenerator(QObject *parent = nullptr);
    ~OffThreadGenerator() override;

    bool generating() const { return generating_; }

    // The thread the most recent run's worker used, or 0 before any run. For
    // the tests to prove the generation actually left the controller's thread.
    quintptr workerThreadId() const { return workerThreadId_.load(); }

signals:
    void generatingChanged();
    // A generation run ended (on the controller's thread): the shell reloads
    // the engine timeline so any newly generated cache lands in the preview.
    void finished();

protected:
    // Runs `work` on a fresh worker thread, off the controller's thread, after
    // recording the running state and resetting the cancel flag and the
    // recorded worker id. Returns false (and runs nothing) when a run is
    // already in flight. The worker may poll `cancelRequested()` to stop
    // early; `finished` fires on the controller's thread when it returns.
    bool start(std::function<void()> work);

    // Runs `work` on a fresh worker thread and hands its result to
    // `completion` on the controller's thread, after the worker is joined and
    // the completion signals have fired. The result travels behind a
    // shared_ptr slot the worker writes and the completion reads, so `Result`
    // need not be default-constructible or copyable - only movable. `Result`
    // is deduced from the work invocable's return type (a lambda has no
    // conversion to `std::function` to deduce from, so the parameter types
    // cannot be spelled as `std::function<Result()>`).
    template <class Work, class Completion>
    bool start(Work &&work, Completion &&completion)
    {
        using Result = std::invoke_result_t<Work>;
        auto slot = std::make_shared<std::optional<Result>>();
        return startImpl(
                [work = std::forward<Work>(work), slot]() mutable { slot->emplace(work()); },
                [completion = std::forward<Completion>(completion), slot]() mutable {
                    completion(std::move(**slot));
                });
    }

    // Whether a destruction asked the running worker to stop. Readable from
    // the worker thread.
    bool cancelRequested() const { return cancelRequested_.load(std::memory_order_relaxed); }

private:
    // Runs `worker` on a fresh worker thread, then queues `completion` on the
    // controller's thread after the worker is joined and the completion
    // signals have fired.
    bool startImpl(std::function<void()> worker, std::function<void()> completion);

    // Runs on the controller's thread when the worker finishes: joins the
    // worker and emits the completion signals.
    void finishGeneration();

    bool generating_ = false;
    std::atomic<bool> cancelRequested_{ false };
    std::atomic<quintptr> workerThreadId_{ 0 };
    std::thread workerThread_;
};
