// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace genesis::jobs {

// ---------------------------------------------------------------------------
// The contract, not the runtime
// ---------------------------------------------------------------------------
//
// This header defines the host-side contract for long-running AI work:
// transcription, speech synthesis, subject detection and enhancement. It is
// deliberately engine-free and model-free - no whisper.cpp, no ONNX, no
// network, no thread. A job carries the host's own request data in and the
// host's own result data out; a real backend (a model runtime or a cloud
// provider) plugs in behind Scheduler later without this contract changing.
//
// Nothing here touches the project. A job that finishes hands its result to
// the caller, who applies it as commands; the job itself never mutates a
// project and never even sees one (docs/architecture.md R10).

// What a job is for: transcribe (whisper.cpp), tts (Kokoro), the cutout
// subject models (RVM person matting / IS-Net object) and enhance
// (Real-ESRGAN).
enum class JobKind {
    // Media audio -> timed caption segments.
    Transcribe,
    // Typed narration -> a WAV in the project folder.
    SynthesizeSpeech,
    // Media -> subject rectangles, one per subject found.
    DetectSubjects,
    // A picture -> a larger, cleaner picture.
    Enhance,
};

// The kind's name, for progress/log lines: "transcribe", ...
constexpr std::string_view job_kind_name(JobKind kind)
{
    switch (kind) {
    case JobKind::Transcribe:
        return "transcribe";
    case JobKind::SynthesizeSpeech:
        return "synthesize-speech";
    case JobKind::DetectSubjects:
        return "detect-subjects";
    case JobKind::Enhance:
        return "enhance";
    }
    return "transcribe";
}

// Where a job is in its life. Done, Failed and Cancelled are terminal.
enum class JobState {
    // Waiting for a free worker slot.
    Queued,
    // A worker is running it.
    Running,
    // Finished with a result.
    Done,
    // The worker could not do the work; see Job::failure.
    Failed,
    // Cancelled before a result, queued or running.
    Cancelled,
};

// The state's name, for log lines: "queued", "running", ...
constexpr std::string_view job_state_name(JobState state)
{
    switch (state) {
    case JobState::Queued:
        return "queued";
    case JobState::Running:
        return "running";
    case JobState::Done:
        return "done";
    case JobState::Failed:
        return "failed";
    case JobState::Cancelled:
        return "cancelled";
    }
    return "queued";
}

constexpr bool is_terminal(JobState state)
{
    return state == JobState::Done || state == JobState::Failed || state == JobState::Cancelled;
}

// A job's stable handle into the scheduler. Minted by Scheduler::submit and
// never re-issued. Distinct from the project's id mint: a job is not part of
// the project, so its ids are the scheduler's own concern.
using JobId = std::uint64_t;

// Progress toward a result: a fraction in 0..1 plus a human-legible stage
// ("downloading model", "transcribing", ...). The fraction drives a bar; the
// stage is what the user reads.
struct Progress
{
    double fraction = 0.0;
    std::string stage;
};

// --- Request payloads: the host's own data, one per kind -------------------

// What to transcribe: a media file,
// an audio stream, and the source window the clip covers.
struct TranscribeRequest
{
    // The media file to transcribe.
    std::string path;
    // Which audio stream, by index; nullopt is the first - the same choice
    // the clip's playback makes, so the words come from the heard track.
    std::optional<std::uint32_t> audio_stream;
    // Seconds into the file where the window begins.
    double source_start = 0.0;
    // How much source the window covers, in seconds (duration * speed).
    double window = 0.0;
    // Which model, e.g. "base.en".
    std::string model_id;
};

// What to speak: typed narration into a WAV. The optional tuning knobs
// (pauses, steps, reference audio) are a provider detail the contract does
// not model.
struct SynthesizeSpeechRequest
{
    // Which model, e.g. "kokoro-int8-multi-lang-v1_0".
    std::string model_id;
    // The voice, from the model's voice table.
    std::int32_t voice = 0;
    // What to say.
    std::string text;
    // Speaking rate; 1.0 is the voice's natural pace.
    double speed = 1.0;
    // The project folder the WAV lands in.
    std::string project;
};

// Which subject the cutout model looks for. Kept here so the contract needs
// no project type.
enum class Subject {
    // A person when the person model finds one, the object otherwise.
    Auto,
    // The person model, always.
    Person,
    // The object model, always.
    Object,
};

constexpr std::string_view subject_name(Subject subject)
{
    switch (subject) {
    case Subject::Auto:
        return "auto";
    case Subject::Person:
        return "person";
    case Subject::Object:
        return "object";
    }
    return "auto";
}

// What to find subjects in.
struct DetectSubjectsRequest
{
    std::string path;
    Subject subject = Subject::Auto;
    std::string model_id;
};

// What to enhance.
struct EnhanceRequest
{
    std::string path;
    // How many times bigger to make it.
    std::uint32_t factor = 2;
};

// One request: the kind is the payload's alternative, so a request can never
// disagree with its own kind (the one source of truth the Command type uses
// too). Construct it as `JobRequest{TranscribeRequest{...}}`.
struct JobRequest
{
    std::variant<TranscribeRequest, SynthesizeSpeechRequest, DetectSubjectsRequest, EnhanceRequest>
            payload;

    JobRequest() = default;

    template <typename T>
    JobRequest(T &&alternative) : payload(std::forward<T>(alternative))
    {
    }
};

// The kind a request asks for.
inline JobKind job_kind(const JobRequest &request)
{
    return std::visit(
            [](const auto &params) {
                using T = std::decay_t<decltype(params)>;
                if constexpr (std::is_same_v<T, TranscribeRequest>) {
                    return JobKind::Transcribe;
                } else if constexpr (std::is_same_v<T, SynthesizeSpeechRequest>) {
                    return JobKind::SynthesizeSpeech;
                } else if constexpr (std::is_same_v<T, DetectSubjectsRequest>) {
                    return JobKind::DetectSubjects;
                } else {
                    return JobKind::Enhance;
                }
            },
            request.payload);
}

// --- Result payloads: the host's own data, never an engine or model type ----

// One caption, in seconds relative to the transcribed window's start.
struct CaptionSegment
{
    double start = 0.0;
    double end = 0.0;
    std::string text;
};

// A transcript: the timed caption segments, in time order.
struct TranscriptResult
{
    std::vector<CaptionSegment> segments;
};

// What synthesis produced.
struct SpeechResult
{
    // Absolute path of the written WAV, ready for the media import path.
    std::string path;
    // Seconds of audio, so the caller can say something useful.
    double duration = 0.0;
};

// One subject the detector found: its box as fractions of the frame, so the
// same result means the same subject at any output size.
struct SubjectRect
{
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    Subject subject = Subject::Auto;
};

// The subjects found, in no particular order.
struct SubjectsResult
{
    std::vector<SubjectRect> rects;
};

// What enhancement produced.
struct EnhanceResult
{
    // Absolute path of the written media.
    std::string path;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// One result: the payload for the job's kind. Which alternative holds is the
// caller's knowledge - the job's kind names it.
struct JobResult
{
    std::variant<TranscriptResult, SpeechResult, SubjectsResult, EnhanceResult> payload;

    JobResult() = default;

    template <typename T>
    JobResult(T &&alternative) : payload(std::forward<T>(alternative))
    {
    }
};

// The unit of long-running work. Owned by the scheduler; handed out read-only
// by Scheduler::poll and moved between states only through the scheduler, so
// the transitions below are the only ones that can happen:
//
//   Queued -> Running         (Scheduler::acquire starts it)
//   Queued -> Cancelled       (Scheduler::cancel, never run)
//   Running -> Done           (Scheduler::complete with a result)
//   Running -> Failed         (Scheduler::fail with a reason)
//   Running -> Cancelled      (Scheduler::cancel, then complete/fail)
//
// No other transition is possible. A refused transition is a no-op that
// leaves the job untouched, never a throw: a failed job is a state (Failed),
// not an exception.
class Job
{
public:
    Job(JobId id, JobRequest request);

    JobId id() const { return id_; }
    JobKind kind() const { return job_kind(request_); }
    const JobRequest &request() const { return request_; }
    JobState state() const { return state_; }

    // The latest progress; the fraction is held in 0..1.
    double progress_fraction() const { return progress_.fraction; }
    const std::string &stage() const { return progress_.stage; }

    // The result, present only when the job is Done.
    const std::optional<JobResult> &result() const { return result_; }
    // Why the job failed, present only when it is Failed.
    const std::optional<std::string> &failure() const { return failure_; }

    // Whether the caller asked to cancel. A worker polls this between units
    // of work and stops early when it turns true.
    bool cancel_requested() const { return cancel_requested_; }

    bool is_terminal() const { return genesis::jobs::is_terminal(state_); }

private:
    friend class Scheduler;

    // Queued -> Running. Refused (false) unless the job is Queued.
    bool start();
    // Records progress while Running; the fraction is clamped to 0..1.
    // Refused unless Running.
    bool report_progress(Progress progress);
    // Running -> Done with the result, progress pinned to 1.0. When a cancel
    // was requested first, Running -> Cancelled instead and the result is
    // discarded. Refused unless Running.
    bool complete(JobResult result);
    // Running -> Failed with the reason, no result. When a cancel was
    // requested first, Running -> Cancelled instead. Refused unless Running.
    bool fail(std::string reason);
    // Requests cancellation: Queued -> Cancelled at once; Running only sets
    // the flag for the worker to observe (the terminal transition happens on
    // the worker's next complete/fail). No effect on a terminal job.
    bool request_cancel();

    JobId id_;
    JobRequest request_;
    JobState state_ = JobState::Queued;
    Progress progress_;
    std::optional<JobResult> result_;
    std::optional<std::string> failure_;
    bool cancel_requested_ = false;
};

} // namespace genesis::jobs
