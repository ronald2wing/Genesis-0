// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

// The measurable half of Phase 9 (release hardening): host-side, engine-free
// benchmarks over a synthetically generated project. No GPU, no display, no
// Qt - only what the project/render layers already provide, so a regression
// can be caught without a window or an engine (see docs/rewrite-plan.md Phase
// 9: "performance on reference hardware").
//
// Timing uses std::chrono::steady_clock rather than a benchmark framework
// (Catch2's benchmark, Google Benchmark, ...). No framework is approved yet;
// the scenario registry and distribution reporting here are the seam a
// framework could later replace without touching the scenarios. This is a
// dependency decision deferred, not taken - no new dependency is introduced.

namespace genesis::perf {

// A per-iteration duration distribution, in microseconds. p50/p95 use the
// nearest-rank percentile over the timed iterations; max is the slowest.
struct Distribution
{
    std::int64_t p50_us = 0;
    std::int64_t p95_us = 0;
    std::int64_t max_us = 0;
    std::size_t iterations = 0;

    bool operator==(const Distribution &) const = default;
};

// One scenario: the setup runs once, the body once per timed iteration, the
// teardown once at the end. The body returns the work it did (clips
// flattened, edits applied, positions resolved) so the runner can prove a
// scenario actually ran rather than timed an empty loop.
struct Scenario
{
    std::string name;
    std::function<void()> setup;
    std::function<std::uint64_t()> body;
    std::function<void()> teardown;
};

// A scenario factory: a project size in clips to a concrete scenario. A
// benchmark is meaningful at 100 clips and at 10 000 because the fixture is
// generated through the real command layer at the caller's size.
struct Benchmark
{
    std::string name;
    std::function<Scenario(std::size_t)> make;
};

// Every measurable benchmark, in table order.
std::vector<Benchmark> benchmarks();

// The result of running a scenario: the distribution and the total work.
struct RunResult
{
    Distribution dist;
    std::uint64_t work = 0;
};

// Runs a scenario: `warmup` untimed iterations first (cache and branch
// predictors), then `iterations` timed ones. Throws std::invalid_argument
// when `iterations == 0` - a zero-iteration run reports nothing, so it is
// refused rather than silently emitting an empty distribution.
RunResult run_scenario(const Scenario &scenario, std::size_t warmup, std::size_t iterations);

// One scenario's result, keyed by name and size for the baseline.
struct Result
{
    std::string name;
    std::size_t size = 0;
    Distribution dist;
    std::uint64_t work = 0;
};

// The baseline key for a result: "name@size", e.g. "frame_plan@10000".
std::string baseline_key(const Result &result);

// ---------------------------------------------------------------------------
// Baseline format
//
// A baseline is a plain-text file, one line per scenario result:
//
//     <name>@<size> <p50_us> <p95_us> <max_us>
//
// e.g. "frame_plan@10000 1234 1456 2000". Lines beginning with '#' are
// ignored. p95 is the regression signal: --check fails a scenario whose p95
// exceeds the baseline p95 by more than the caller's percentage threshold.
// A baseline records the *shape* of the run, not a single number, so a
// regression in the tail (a stall) is as visible as one in the median.
// ---------------------------------------------------------------------------

// The baseline a set of results defines.
std::map<std::string, Distribution> baseline_of(const std::vector<Result> &results);

// Reads a baseline file; a line it cannot parse is skipped.
std::map<std::string, Distribution> read_baseline(const std::string &path);

// Writes a baseline file (see the format above).
void write_baseline(const std::string &path, const std::vector<Result> &results);

// Compares results against a baseline. A scenario regresses when its p95
// exceeds the baseline p95 by more than `threshold_pct` (e.g. 25.0 for
// 25%). Returns false when any scenario regresses; `report` lists the
// offending lines. A result with no baseline entry is a regression too - it
// cannot be verified - while a baseline entry with no result is ignored.
bool check_against(const std::vector<Result> &results,
                   const std::map<std::string, Distribution> &baseline, double threshold_pct,
                   std::string &report);

} // namespace genesis::perf
