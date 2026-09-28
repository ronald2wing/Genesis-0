// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "perf/Benchmarks.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "core/ClipTimeline.h"
#include "project/Document.h"
#include "project/Editor.h"
#include "render/Flatten.h"
#include "render/Resolve.h"

namespace genesis::perf {

using genesis::core::FrameRate;
using genesis::core::Rational;
using genesis::core::Timeline;
using genesis::core::TrackId;
using genesis::project::AddClip;
using genesis::project::AddMedia;
using genesis::project::Command;
using genesis::project::DocumentSettings;
using genesis::project::Editor;
using genesis::project::MediaKind;
using genesis::project::NewMedia;
using genesis::project::Project;

namespace {

constexpr std::uint32_t kWidth = 1920;
constexpr std::uint32_t kHeight = 1080;

// The number of edits the undo scenario applies and undoes. Bounded by
// History::DEPTH (200): beyond that the oldest steps are evicted, so more
// edits would not all be undoable.
constexpr std::size_t kUndoEdits = 200;

// The number of positions the scrub scenario resolves. Fixed rather than
// scaled with the timeline so the two sizes are comparable per query.
constexpr std::size_t kScrubPositions = 512;

// Applies a fixture command whose success is a precondition, not something
// checked per iteration: `apply` is [[nodiscard]], so the result must be
// bound, and a deterministic add on a freshly minted id cannot fail here. A
// silently failed add would be caught by the driver's work-count asserts.
void apply_fixture(Editor &editor, const Command &command)
{
    const auto result = editor.apply(command);
    (void)result;
}

// A project of `clips` one-second video clips round-robined over four
// tracks, built through the real command layer so the fixture cannot drift
// from the model. Deterministic: no clock, no randomness - the same command
// sequence mints the same ids every time.
Editor make_project(std::size_t clips)
{
    Editor editor;
    NewMedia item;
    item.path = "/perf/clip.mp4";
    item.name = "clip.mp4";
    item.duration = Rational{ 1, 1 };
    item.kind = MediaKind::Video;
    item.width = kWidth;
    item.height = kHeight;
    item.frame_rate = 30.0;
    item.frame_rate_fraction = "30/1";
    item.has_audio = false;
    const auto added = editor.apply(Command{ AddMedia{ std::move(item) } });
    const std::string media_id = added->created_id.value();
    const auto &tracks = editor.project().timelines[0].tracks;
    for (std::size_t i = 0; i < clips; ++i) {
        const std::string track_id = tracks[i % tracks.size()].id;
        const Rational start{ static_cast<std::int64_t>(i / tracks.size()), 1 };
        apply_fixture(editor, Command{ AddClip{ media_id, track_id, start, false } });
    }
    return editor;
}

// The request the frame-plan and scrub scenarios resolve into.
genesis::render::ExportRequest request()
{
    genesis::render::ExportRequest req;
    req.width = kWidth;
    req.height = kHeight;
    req.rate_num = 30;
    req.rate_den = 1;
    return req;
}

// State a scenario's setup builds and its body reads across iterations.
struct Fixture
{
    Editor editor;
    // The whole timeline resolved once, so the scrub body measures only the
    // per-position queries, not the graph build.
    std::optional<genesis::render::BuiltTimeline> built;
    std::vector<TrackId> tracks;
};

// frame plan: flatten the whole timeline and build the engine timeline. The
// host graph build every scrub and every export frame starts from.
Scenario frame_plan(std::size_t clips)
{
    auto fixture = std::make_shared<Fixture>();
    return Scenario{
        "frame_plan",
        [fixture, clips] { fixture->editor = make_project(clips); },
        [fixture] {
            const std::vector<genesis::render::ExportClip> flat =
                    genesis::render::flatten_timeline(fixture->editor.project(), std::nullopt);
            const genesis::render::ExportRequest req = request();
            const genesis::render::BuiltTimeline built = genesis::render::build_timeline(
                    req, FrameRate::THIRTY, std::span<const genesis::render::ExportClip>(flat),
                    { });
            return static_cast<std::uint64_t>(built.timeline.clip_count());
        },
        [] { },
    };
}

// undo: apply N edits then undo them all, on a project that already holds
// `clips` clips. The command stack's restore returns the exact base state
// (id counter included), so the same ids are minted every iteration.
Scenario undo(std::size_t clips)
{
    auto fixture = std::make_shared<Fixture>();
    return Scenario{
        "undo",
        [fixture, clips] { fixture->editor = make_project(clips); },
        [fixture] {
            Editor &editor = fixture->editor;
            const std::string media_id = editor.project().media[0].id;
            const auto &tracks = editor.project().timelines[0].tracks;
            const std::size_t base = editor.project().timelines[0].clips.size();
            for (std::size_t i = 0; i < kUndoEdits; ++i) {
                const std::string track_id = tracks[i % tracks.size()].id;
                const Rational start{ static_cast<std::int64_t>((base + i) / tracks.size()), 1 };
                apply_fixture(editor, Command{ AddClip{ media_id, track_id, start, false } });
            }
            for (std::size_t i = 0; i < kUndoEdits; ++i) {
                editor.undo();
            }
            return static_cast<std::uint64_t>(kUndoEdits);
        },
        [] { },
    };
}

// file open: write the document and read it back - a save and an open,
// before any media is touched.
Scenario file_open(std::size_t clips)
{
    auto fixture = std::make_shared<Fixture>();
    return Scenario{
        "file_open",
        [fixture, clips] { fixture->editor = make_project(clips); },
        [fixture] {
            const DocumentSettings settings{ "perf", kWidth, kHeight, 30, 1 };
            const nlohmann::json doc =
                    genesis::project::to_document(fixture->editor.project(), settings);
            const std::optional<Project> back = genesis::project::from_document(doc, settings);
            if (!back) {
                return std::uint64_t{ 0 };
            }
            return static_cast<std::uint64_t>(back->active().clips.size());
        },
        [] { },
    };
}

// scrub plan: the host half of a scrub. Resolve the whole timeline once,
// then ask which clips are visible at many positions across it - what the
// playhead does as it moves, without any decode.
Scenario scrub(std::size_t clips)
{
    auto fixture = std::make_shared<Fixture>();
    return Scenario{
        "scrub",
        [fixture, clips] {
            fixture->editor = make_project(clips);
            const std::vector<genesis::render::ExportClip> flat =
                    genesis::render::flatten_timeline(fixture->editor.project(), std::nullopt);
            const genesis::render::ExportRequest req = request();
            fixture->built = genesis::render::build_timeline(
                    req, FrameRate::THIRTY, std::span<const genesis::render::ExportClip>(flat),
                    { });
            fixture->tracks = fixture->built->timeline.track_ids();
        },
        [fixture] {
            const Timeline &timeline = fixture->built->timeline;
            const Rational duration = timeline.duration();
            const auto &tracks = fixture->tracks;
            std::uint64_t visible = 0;
            if (duration.is_zero()) {
                return visible;
            }
            for (std::size_t p = 0; p < kScrubPositions; ++p) {
                const Rational time = duration
                        * Rational{ static_cast<std::int64_t>(p),
                                    static_cast<std::int64_t>(kScrubPositions) };
                for (const TrackId track : tracks) {
                    if (timeline.clip_on_track_at(track, time)) {
                        ++visible;
                    }
                }
            }
            return visible;
        },
        [] { },
    };
}

} // namespace

std::vector<Benchmark> benchmarks()
{
    return {
        { "frame_plan", frame_plan },
        { "undo", undo },
        { "file_open", file_open },
        { "scrub", scrub },
    };
}

RunResult run_scenario(const Scenario &scenario, std::size_t warmup, std::size_t iterations)
{
    if (iterations == 0) {
        throw std::invalid_argument("a zero-iteration scenario run reports nothing");
    }
    scenario.setup();
    for (std::size_t i = 0; i < warmup; ++i) {
        (void)scenario.body();
    }

    std::vector<std::int64_t> samples;
    samples.reserve(iterations);
    std::uint64_t work = 0;
    for (std::size_t i = 0; i < iterations; ++i) {
        const auto start = std::chrono::steady_clock::now();
        work += scenario.body();
        const auto end = std::chrono::steady_clock::now();
        samples.push_back(
                std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
    }
    scenario.teardown();

    std::sort(samples.begin(), samples.end());
    const auto percentile = [&samples](double p) {
        const std::size_t rank =
                static_cast<std::size_t>(std::ceil(p * static_cast<double>(samples.size())));
        const std::size_t index = rank == 0 ? 0 : rank - 1;
        return samples[std::min(index, samples.size() - 1)];
    };

    Distribution dist;
    dist.p50_us = percentile(0.50);
    dist.p95_us = percentile(0.95);
    dist.max_us = samples.back();
    dist.iterations = iterations;
    return RunResult{ dist, work };
}

std::string baseline_key(const Result &result)
{
    return result.name + "@" + std::to_string(result.size);
}

std::map<std::string, Distribution> baseline_of(const std::vector<Result> &results)
{
    std::map<std::string, Distribution> baseline;
    for (const Result &result : results) {
        baseline[baseline_key(result)] = result.dist;
    }
    return baseline;
}

void write_baseline(const std::string &path, const std::vector<Result> &results)
{
    std::ofstream out(path);
    out << "# genesis::perf baseline - p50_us p95_us max_us per iteration\n";
    for (const Result &result : results) {
        out << baseline_key(result) << ' ' << result.dist.p50_us << ' ' << result.dist.p95_us << ' '
            << result.dist.max_us << '\n';
    }
}

std::map<std::string, Distribution> read_baseline(const std::string &path)
{
    std::map<std::string, Distribution> baseline;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream fields(line);
        std::string key;
        std::int64_t p50 = 0;
        std::int64_t p95 = 0;
        std::int64_t max = 0;
        if (!(fields >> key >> p50 >> p95 >> max)) {
            continue;
        }
        Distribution dist;
        dist.p50_us = p50;
        dist.p95_us = p95;
        dist.max_us = max;
        baseline[key] = dist;
    }
    return baseline;
}

bool check_against(const std::vector<Result> &results,
                   const std::map<std::string, Distribution> &baseline, double threshold_pct,
                   std::string &report)
{
    bool holds = true;
    std::ostringstream out;
    for (const Result &result : results) {
        const std::string key = baseline_key(result);
        const auto it = baseline.find(key);
        if (it == baseline.end()) {
            holds = false;
            out << key << ": no baseline recorded\n";
            continue;
        }
        const double limit =
                static_cast<double>(it->second.p95_us) * (1.0 + (threshold_pct / 100.0));
        if (static_cast<double>(result.dist.p95_us) > limit) {
            holds = false;
            out << key << ": p95 " << result.dist.p95_us << "us over " << it->second.p95_us
                << "us baseline (+" << threshold_pct << "%)\n";
        }
    }
    report = out.str();
    return holds;
}

} // namespace genesis::perf
