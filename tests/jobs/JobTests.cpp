// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The AI job contract's suite. Drives a scheduler with a fake worker and no
// threads: the scheduler is caller-pumped, so a test submits, acquires and
// completes jobs inline, deterministically.

#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "jobs/Job.h"
#include "jobs/Scheduler.h"
#include "project/Project.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/model/Cutout.h"
#include "project/model/Media.h"
#include "project/Editor.h"

namespace {

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

namespace jobs = genesis::jobs;
namespace project = genesis::project;

using jobs::CaptionSegment;
using jobs::Job;
using jobs::JobId;
using jobs::JobKind;
using jobs::JobRequest;
using jobs::JobResult;
using jobs::JobState;
using jobs::Progress;
using jobs::Scheduler;
using jobs::Subject;
using jobs::TranscriptResult;

// A transcribe request over a synthetic file - the request the fake worker
// reads, never touching a real file or model.
JobRequest transcribe_request()
{
    jobs::TranscribeRequest params;
    params.path = "/footage/a.mp4";
    params.window = 10.0;
    params.model_id = "base.en";
    return JobRequest{ std::move(params) };
}

// The transcript a fake worker would produce from that request.
JobResult transcript(std::vector<CaptionSegment> segments)
{
    TranscriptResult payload;
    payload.segments = std::move(segments);
    return JobResult{ std::move(payload) };
}

void test_a_submitted_job_runs_to_done_with_a_result()
{
    Scheduler scheduler;
    const JobId id = scheduler.submit(transcribe_request());
    check(scheduler.poll(id)->state() == JobState::Queued, "a submitted job is queued");

    const std::optional<JobId> acquired = scheduler.acquire();
    check(acquired.has_value() && *acquired == id, "acquire starts the queued job");
    check(scheduler.poll(id)->state() == JobState::Running, "the job is now running");

    check(scheduler.complete(id, transcript({ { 0.0, 2.0, "hello" } })),
          "complete is accepted while running");

    const Job done = *scheduler.poll(id);
    check(done.state() == JobState::Done, "the job is done");
    check(done.is_terminal(), "done is terminal");
    check(done.progress_fraction() == 1.0, "done ends at full progress");
    check(done.result().has_value(), "done carries a result");
    check(!done.failure(), "done carries no failure");

    const TranscriptResult &transcript_done = std::get<TranscriptResult>(done.result()->payload);
    check(transcript_done.segments.size() == 1 && transcript_done.segments[0].text == "hello",
          "the result payload is the transcript itself");
}

void test_progress_arrives_in_order_and_ends_at_one()
{
    Scheduler scheduler;
    const JobId id = scheduler.submit(transcribe_request());
    scheduler.acquire();

    check(scheduler.report_progress(id, Progress{ 0.25, "loading model" }),
          "first progress accepted");
    check(scheduler.report_progress(id, Progress{ 0.5, "transcribing" }),
          "second progress accepted");
    check(scheduler.report_progress(id, Progress{ 0.75, "transcribing" }),
          "third progress accepted");
    check(scheduler.poll(id)->progress_fraction() == 0.75, "progress holds the latest report");
    check(scheduler.poll(id)->stage() == "transcribing", "the stage is human-legible");

    // An overshoot is clamped to 1.0, never beyond it.
    check(scheduler.report_progress(id, Progress{ 1.5, "finishing" }),
          "an overshooting report is accepted");
    check(scheduler.poll(id)->progress_fraction() == 1.0, "the fraction clamps to 1.0");

    scheduler.complete(id, transcript({ { 0.0, 1.0, "hi" } }));
    check(scheduler.poll(id)->progress_fraction() == 1.0, "a finished job ends at 1.0");
}

void test_cancelling_a_queued_job_never_runs_it()
{
    Scheduler scheduler;
    const JobId id = scheduler.submit(transcribe_request());
    scheduler.cancel(id);
    check(scheduler.poll(id)->state() == JobState::Cancelled, "a queued job cancels at once");
    check(!scheduler.acquire().has_value(), "the cancelled job is never handed to a worker");
    check(scheduler.running_count() == 0 && scheduler.queued_count() == 0,
          "nothing runs and nothing waits");
}

void test_cancelling_a_running_job()
{
    Scheduler scheduler;
    const JobId id = scheduler.submit(transcribe_request());
    scheduler.acquire();

    scheduler.cancel(id);
    check(scheduler.poll(id)->state() == JobState::Running, "cancel only flags a running job");
    check(scheduler.cancel_requested(id), "the worker can see the cancel");

    // The worker finishes its current unit and reports; the result lands as
    // Cancelled and is discarded.
    check(scheduler.complete(id, transcript({ { 0.0, 1.0, "never applied" } })),
          "complete after cancel is accepted");
    check(scheduler.poll(id)->state() == JobState::Cancelled, "the job ends cancelled");
    check(!scheduler.poll(id)->result(), "the result is discarded");
}

void test_a_worker_failure_yields_failed_with_a_reason()
{
    Scheduler scheduler;
    const JobId id = scheduler.submit(transcribe_request());
    scheduler.acquire();

    check(scheduler.fail(id, "model not installed"), "fail is accepted while running");
    const Job failed = *scheduler.poll(id);
    check(failed.state() == JobState::Failed, "the job failed");
    check(failed.failure().has_value() && *failed.failure() == "model not installed",
          "the reason is recorded");
    check(!failed.result(), "a failed job carries no result");
}

void test_illegal_transitions_are_refused()
{
    Scheduler scheduler;
    const JobId id = scheduler.submit(transcribe_request());

    // Reading a result before Done is refused: there is none.
    check(!scheduler.poll(id)->result(), "no result before done");

    // A queued job cannot complete, fail or report progress.
    check(!scheduler.complete(id, transcript({ })), "complete refused while queued");
    check(!scheduler.fail(id, "too soon"), "fail refused while queued");
    check(!scheduler.report_progress(id, Progress{ 0.5, "early" }),
          "progress refused while queued");
    check(scheduler.poll(id)->state() == JobState::Queued, "refusals leave the job queued");

    // A terminal job cannot be finished again or started again.
    scheduler.acquire();
    scheduler.complete(id, transcript({ { 0.0, 1.0, "done" } }));
    check(scheduler.poll(id)->state() == JobState::Done, "the job finished");
    check(!scheduler.fail(id, "after the fact"), "fail refused after done");
    check(!scheduler.complete(id, transcript({ })), "a second complete is refused");
    check(!scheduler.acquire().has_value(), "a done job is not re-acquired");
}

void test_concurrency_is_bounded()
{
    Scheduler scheduler(2);
    const JobId a = scheduler.submit(transcribe_request());
    const JobId b = scheduler.submit(transcribe_request());
    const JobId c = scheduler.submit(transcribe_request());

    const std::optional<JobId> first = scheduler.acquire();
    const std::optional<JobId> second = scheduler.acquire();
    check(first.has_value() && *first == a, "the first slot starts job a");
    check(second.has_value() && *second == b, "the second slot starts job b");
    check(!scheduler.acquire().has_value(), "no third slot while two run");
    check(scheduler.running_count() == 2 && scheduler.queued_count() == 1, "two run and one waits");

    scheduler.complete(a, transcript({ { 0.0, 1.0, "a" } }));
    const std::optional<JobId> third = scheduler.acquire();
    check(third.has_value() && *third == c, "a freed slot starts job c");
    scheduler.complete(b, transcript({ { 0.0, 1.0, "b" } }));
    scheduler.complete(c, transcript({ { 0.0, 1.0, "c" } }));
    check(scheduler.running_count() == 0 && scheduler.queued_count() == 0, "every job finished");
}

void test_a_job_never_mutates_the_project()
{
    project::Editor editor;
    // A real project built through the command layer, so the fixture is a
    // genuine document and not a stand-in.
    project::NewMedia item;
    item.path = "/footage/a.mp4";
    item.name = "a.mp4";
    item.duration = project::Rational{ 10, 1 };
    item.kind = project::MediaKind::Video;
    item.width = 1920;
    item.height = 1080;
    item.has_audio = true;
    const auto add_media = editor.apply(project::Command{ project::AddMedia{ std::move(item) } });
    check(add_media.has_value(), "adds media");

    const project::Project before = editor.project();

    // A job runs to completion over the project's own media; its result is
    // the caller's to apply, and the scheduler never sees the project.
    Scheduler scheduler;
    const JobId id = scheduler.submit(transcribe_request());
    scheduler.acquire();
    scheduler.complete(id, transcript({ { 0.0, 10.0, "the whole clip" } }));
    check(scheduler.poll(id)->state() == JobState::Done, "the job finished");

    check(editor.project() == before, "running a job does not mutate the project");
}

void test_every_kind_reports_its_own_payload()
{
    check(jobs::job_kind_name(JobKind::Transcribe) == "transcribe", "transcribe is named");
    check(jobs::job_state_name(JobState::Cancelled) == "cancelled", "cancelled is named");
    check(jobs::is_terminal(JobState::Done), "done is terminal");
    check(!jobs::is_terminal(JobState::Queued), "queued is not terminal");

    // Subject detection: the request and result carry the host's own data,
    // and the kind derives from the payload.
    jobs::DetectSubjectsRequest detect;
    detect.path = "/footage/a.mp4";
    detect.subject = Subject::Person;
    detect.model_id = "rvm-mobilenetv3";

    jobs::SubjectsResult subjects;
    subjects.rects.push_back(jobs::SubjectRect{ 0.1, 0.1, 0.4, 0.4, Subject::Person });
    check(jobs::subject_name(Subject::Person) == "person", "a subject is named");

    Scheduler scheduler;
    const JobId id = scheduler.submit(JobRequest{ std::move(detect) });
    check(scheduler.poll(id)->kind() == JobKind::DetectSubjects, "the kind comes from the payload");
    scheduler.acquire();
    scheduler.complete(id, JobResult{ std::move(subjects) });

    const Job done = *scheduler.poll(id);
    check(done.state() == JobState::Done, "the detection finished");
    const jobs::SubjectsResult &found = std::get<jobs::SubjectsResult>(done.result()->payload);
    check(found.rects.size() == 1 && found.rects[0].width == 0.4,
          "the result holds the subject rectangles");
}

} // namespace

int main()
{
    test_a_submitted_job_runs_to_done_with_a_result();
    test_progress_arrives_in_order_and_ends_at_one();
    test_cancelling_a_queued_job_never_runs_it();
    test_cancelling_a_running_job();
    test_a_worker_failure_yields_failed_with_a_reason();
    test_illegal_transitions_are_refused();
    test_concurrency_is_bounded();
    test_a_job_never_mutates_the_project();
    test_every_kind_reports_its_own_payload();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
