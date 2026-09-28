// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The app's real ai::Fetcher: streams one model download over
// QNetworkAccessManager on the calling thread (the caption worker's), trying
// the mirror before the upstream URL (docs/decisions/ai-provider.md §3(c)) and
// aborting when the caller's cancel flag turns true or the store's sink asks
// it to stop. It owns no Qt state across calls - the manager and reply live
// inside one operator() - so the same instance is safe to hand to any thread
// that will perform the download, and QNetworkAccessManager is constructed on
// the thread that actually runs the transfer.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "ai/ModelStore.h"

// A transport for one model download, matching ai::Fetcher's call signature so
// an instance drops straight into ai::install. It is deliberately Qt-free in
// the header: only the .cpp sees QNetworkAccessManager, so the app's GES-before
// -Qt include ordering is untouched by including this.
class QtFetcher
{
public:
    // The sink shape the store hands in; named so the callable reads cleanly.
    using Sink = std::function<bool(std::span<const std::uint8_t>)>;

    // `cancel` is polled between reads; null disables the watchdog (a run that
    // can never be cancelled mid-transfer).
    explicit QtFetcher(const std::atomic<bool> *cancel = nullptr);

    // Downloads `entry`'s mirror then upstream, feeding every byte through
    // `sink`. Returns true when a full body was delivered (the store verifies
    // the digest), false when the transfer was interrupted, the sink stopped
    // it, or the caller cancelled. A failed mirror falls through to the
    // upstream; a sink-stop or a cancel does not.
    bool operator()(const genesis::ai::ModelEntry &entry, const Sink &sink) const;

private:
    enum class Transfer { Complete, StoreStopped, Failed, Cancelled };

    Transfer transfer(const std::string &url, const Sink &sink) const;

    const std::atomic<bool> *cancel_ = nullptr;
};
