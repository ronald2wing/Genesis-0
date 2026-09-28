// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "api/Api.h"

class AiController;
class CutoutController;
class EnhanceController;
class TtsController;

// One download the seam runs off the calling thread (the UI thread for a
// panel's call, so the fetch must not block it). The worker installs every
// model the kind needs through ai::install; `active` stays true until it
// finishes so `ai.status` can report "downloading". The destructor cancels and
// joins, so a kind's download never outlives the app or trips std::terminate on
// a joinable thread at shutdown.
struct AiDownloadSlot
{
    std::atomic<bool> active{ false };
    std::atomic<bool> cancel{ false };
    std::atomic<double> progress{ 0.0 };
    std::atomic<bool> failed{ false };
    // Mutable so last_error() (const, called from the status projection) can
    // lock it.
    mutable std::mutex error_mutex;
    std::string error;
    std::thread worker;

    ~AiDownloadSlot()
    {
        cancel.store(true, std::memory_order_relaxed);
        if (worker.joinable()) {
            worker.join();
        }
    }

    // Joins a finished worker before the slot is reused, so a kind never has
    // two download threads and a previous run's thread is reaped.
    void reap()
    {
        if (worker.joinable() && !active.load(std::memory_order_relaxed)) {
            worker.join();
        }
    }

    std::string last_error() const
    {
        std::lock_guard<std::mutex> lock(error_mutex);
        return error;
    }
};

using AiDownloadSlots = std::map<std::string, std::shared_ptr<AiDownloadSlot>>;

// The ai.* dispatch body: one verb per `AiRequest` alternative, over the four
// controllers. Runs on the calling thread (dispatch does not marshal), so every
// verb only touches the controllers' public surface and never blocks on the
// workers; `ai.download` hands its install to a detached worker thread.
genesis::api::Response ai_service(const genesis::api::AiRequest &request, AiController *captions,
                                  CutoutController *cutout, EnhanceController *enhance,
                                  TtsController *tts, const std::filesystem::path &model_root,
                                  const std::shared_ptr<AiDownloadSlots> &ai_slots);