// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "jobs/Scheduler.h"

#include <algorithm>
#include <string>
#include <utility>

namespace genesis::jobs {

Scheduler::Scheduler(std::size_t max_concurrent)
    : max_concurrent_(max_concurrent == 0 ? 1 : max_concurrent)
{
}

JobId Scheduler::submit(JobRequest request)
{
    const JobId id = ++next_id_;
    jobs_.emplace_back(id, std::move(request));
    return id;
}

void Scheduler::cancel(JobId id)
{
    if (Job *job = find(id)) {
        job->request_cancel();
    }
}

std::optional<Job> Scheduler::poll(JobId id) const
{
    const Job *job = find(id);
    if (!job) {
        return std::nullopt;
    }
    return *job;
}

std::optional<JobId> Scheduler::acquire()
{
    if (running_count() >= max_concurrent_) {
        return std::nullopt;
    }
    // The earliest queued job first, so a queue runs in submission order.
    for (Job &job : jobs_) {
        if (job.state() == JobState::Queued) {
            job.start();
            return job.id();
        }
    }
    return std::nullopt;
}

bool Scheduler::report_progress(JobId id, Progress progress)
{
    Job *job = find(id);
    return job != nullptr && job->report_progress(std::move(progress));
}

bool Scheduler::cancel_requested(JobId id) const
{
    const Job *job = find(id);
    return job != nullptr && job->cancel_requested();
}

bool Scheduler::complete(JobId id, JobResult result)
{
    Job *job = find(id);
    return job != nullptr && job->complete(std::move(result));
}

bool Scheduler::fail(JobId id, std::string reason)
{
    Job *job = find(id);
    return job != nullptr && job->fail(std::move(reason));
}

std::size_t Scheduler::running_count() const
{
    return static_cast<std::size_t>(std::count_if(jobs_.begin(), jobs_.end(), [](const Job &job) {
        return job.state() == JobState::Running;
    }));
}

std::size_t Scheduler::queued_count() const
{
    return static_cast<std::size_t>(std::count_if(jobs_.begin(), jobs_.end(), [](const Job &job) {
        return job.state() == JobState::Queued;
    }));
}

Job *Scheduler::find(JobId id)
{
    const auto it = std::find_if(jobs_.begin(), jobs_.end(),
                                 [id](const Job &job) { return job.id() == id; });
    return it != jobs_.end() ? &*it : nullptr;
}

const Job *Scheduler::find(JobId id) const
{
    const auto it = std::find_if(jobs_.begin(), jobs_.end(),
                                 [id](const Job &job) { return job.id() == id; });
    return it != jobs_.end() ? &*it : nullptr;
}

} // namespace genesis::jobs
