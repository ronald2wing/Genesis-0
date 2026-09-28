// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "jobs/Job.h"

#include <algorithm>
#include <utility>

namespace genesis::jobs {

Job::Job(JobId id, JobRequest request) : id_(id), request_(std::move(request)) { }

bool Job::start()
{
    if (state_ != JobState::Queued) {
        return false;
    }
    state_ = JobState::Running;
    return true;
}

bool Job::report_progress(Progress progress)
{
    if (state_ != JobState::Running) {
        return false;
    }
    progress.fraction = std::clamp(progress.fraction, 0.0, 1.0);
    progress_ = std::move(progress);
    return true;
}

bool Job::complete(JobResult result)
{
    if (state_ != JobState::Running) {
        return false;
    }
    // A cancel requested mid-run wins over the result: the user asked to
    // stop, and the result is discarded so nothing half-made is applied.
    if (cancel_requested_) {
        state_ = JobState::Cancelled;
        return true;
    }
    state_ = JobState::Done;
    result_ = std::move(result);
    progress_.fraction = 1.0;
    return true;
}

bool Job::fail(std::string reason)
{
    if (state_ != JobState::Running) {
        return false;
    }
    if (cancel_requested_) {
        state_ = JobState::Cancelled;
        return true;
    }
    state_ = JobState::Failed;
    failure_ = std::move(reason);
    return true;
}

bool Job::request_cancel()
{
    switch (state_) {
    case JobState::Queued:
        state_ = JobState::Cancelled;
        return true;
    case JobState::Running:
        cancel_requested_ = true;
        return true;
    default:
        // A terminal job cannot be cancelled; the ask is a no-op.
        return false;
    }
}

} // namespace genesis::jobs
