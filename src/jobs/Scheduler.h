// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Derived from Concat's concat-host job slot (jobs.rs),
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "jobs/Job.h"

namespace genesis::jobs {

// The queue of long-running AI jobs: submit, cancel, observe, and a bounded
// number of jobs running at once. The scheduler owns the state machine; a
// worker - whatever code actually runs a model, supplied by the caller - is
// the only thing that can move a Running job to a terminal state.
//
// Threading contract (R4: no media work on the UI thread). The scheduler is
// caller-pumped and owns no thread. submit, cancel, poll and acquire are not
// internally synchronized: drive the scheduler from a single owner thread.
// The work itself runs wherever the caller runs it - a caller that must keep
// media work off the UI thread calls acquire() from a worker thread and
// reports back through the worker-facing methods below. The scheduler never
// runs a job by itself and never blocks inside acquire(); acquire() only
// claims a queued job and returns, so the caller's thread stays the thing
// that runs it.
//
// A job that finishes hands its result to the caller, who applies it to the
// project as commands; the scheduler never touches the project (it does not
// even see it). No AI runtime, no model, no network lives here: a real
// backend plugs in behind acquire()/complete()/fail() later.
class Scheduler
{
public:
    // At most `max_concurrent` jobs run at once; the rest wait queued. A
    // zero is lifted to one, so a scheduler always makes progress.
    explicit Scheduler(std::size_t max_concurrent = 1);

    // Queues a job and returns its id. Never refuses and never throws: the
    // queue is unbounded and a failure is a job state, not an exception.
    JobId submit(JobRequest request);

    // Requests cancellation of the job. A queued job becomes Cancelled at
    // once and is never handed to a worker; a running job is flagged and ends
    // Cancelled when its worker next completes or fails; a terminal job is a
    // no-op.
    void cancel(JobId id);

    // A snapshot of the job, or nullopt for an unknown id.
    std::optional<Job> poll(JobId id) const;

    // Claims the next queued job, moving it Queued -> Running, when a slot is
    // free (fewer than max_concurrent running). Returns the job's id, or
    // nullopt when nothing can start. The caller then runs the job and calls
    // complete or fail with that id; the scheduler itself never runs it.
    std::optional<JobId> acquire();

    // --- Worker-facing: how the code that acquired a job reports back. ------

    // Records progress for a running job; the fraction is clamped to 0..1.
    // False (refused) when the job is not Running.
    bool report_progress(JobId id, Progress progress);

    // Whether the job has been asked to cancel. A worker polls this between
    // units of work and stops early when it turns true.
    bool cancel_requested(JobId id) const;

    // Finishes a running job with its result: Running -> Done, progress
    // pinned to 1.0. If the job was cancelled first, Running -> Cancelled
    // and the result is discarded. False (refused) when the job is not
    // Running.
    bool complete(JobId id, JobResult result);

    // Finishes a running job that could not do its work: Running -> Failed
    // with the reason, no result. If the job was cancelled first, Running ->
    // Cancelled. False (refused) when the job is not Running.
    bool fail(JobId id, std::string reason);

    // The bound set at construction.
    std::size_t max_concurrent() const { return max_concurrent_; }
    // Jobs currently Running.
    std::size_t running_count() const;
    // Jobs waiting to start.
    std::size_t queued_count() const;

private:
    Job *find(JobId id);
    const Job *find(JobId id) const;

    std::vector<Job> jobs_;
    JobId next_id_ = 0;
    std::size_t max_concurrent_;
};

} // namespace genesis::jobs
