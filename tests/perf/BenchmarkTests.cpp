// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The benchmark driver: runs the measurable scenarios, prints the table, and
// asserts the *contract* of the harness - that every scenario ran and did
// work, that a zero-iteration run is refused, and that --check passes against
// a just-recorded baseline and fails against a perturbed one.
//
// Duration budgets. On top of the contract checks, every scenario carries a
// generous p50 ceiling (kBudgets below), so a real regression fails the run
// while ordinary load flakiness does not. Budgets are generous by design: the
// ceiling is ~4x the dev-host p50 at the millisecond-scale @10000 sizes and
// ~5x at the microsecond-scale @100 sizes, so the gate only trips on a
// slowdown no scheduler noise explains (an accidental O(n^2), a dropped hot
// cache). A budget is a regression ceiling, not a target. The @10000
// scenarios are hard gates - a regression shows up in them. The @100
// scenarios warn but never fail: a sub-millisecond measurement can double on
// a scheduling hiccup, and the @10000 gate already catches the same
// regression at scale (see the rationale beside the table).
//
// Environment tolerance. The ceilings above are calibrated on the dev host; a
// CI runner is ~10x slower (shared, throttled vCPU), so the same ceiling
// would hard-fail every CI run on a healthy build. The hard @10000 gates
// therefore carry a scale factor (see budget_scale() below): 1.0 by default,
// 8.0 when `CI` is set (GitHub Actions sets it), overridable via
// GENESIS_PERF_BUDGET_SCALE. The scale relaxes only the hard gates - the @100
// warn-only fixtures never fail, so scaling them would only silence a WARN
// line, not change the outcome. At 8x the @10000 ceilings become 480ms
// (frame_plan), 640ms (undo), 8s (file_open) and 80ms (scrub). The observed
// CI p50s are ~54ms (frame_plan) and ~206ms (undo), i.e. ~9x and ~3x headroom
// respectively; file_open/scrub were not observed but follow the same ~10x
// CI/dev ratio, so ~3x headroom. The tightest gate still catches any
// order-of-magnitude regression on both hosts (a 10x slowdown is 3.2x the
// undo ceiling on CI and 12.5x on the dev host). If CI variance grows past
// the 3x headroom, raise GENESIS_PERF_BUDGET_SCALE rather than re-tuning a
// per-scenario table off one observation.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "../support/Checks.h"
#include "perf/Benchmarks.h"

namespace {

using genesis::test::check;
// The sizes a scenario must be meaningful at, and the fixed warm-up and
// iteration counts. -O2 is assumed: a -O0 benchmark measures nothing.
const std::vector<std::size_t> kSizes = { 100, 10000 };
const std::size_t kWarmup = 2;
const std::size_t kIterations = 20;
const double kThreshold = 25.0;

// A duration budget: the p50 ceiling a scenario must stay under and whether an
// overshoot fails the run (hard) or only warns. `baseline_us` is the dev-host
// p50 the ceiling was derived from (median over repeated runs), recorded so a
// failure message shows the measured number against both reference and ceiling.
struct Budget
{
    std::int64_t baseline_us;
    std::int64_t ceiling_us;
    bool hard_fail;
};

// Generous by design: ~4x the @10000 p50 (hard gates - millisecond-scale and
// stable, so a real regression shows up there) and ~5x the @100 p50 (warn-only
// - sub-millisecond, so a scheduling hiccup can double them and the @10000
// gate catches the same regression anyway). A missing entry is a harness
// error, not a timing result: a renamed scenario or size fails the run.
const std::map<std::string, Budget> kBudgets = {
    { "frame_plan@100", { 800, 4000, false } },  { "frame_plan@10000", { 80000, 320000, true } },
    { "undo@100", { 4800, 24000, false } },      { "undo@10000", { 4000, 16000, true } },
    { "file_open@100", { 8500, 42500, false } }, { "file_open@10000", { 818000, 3200000, true } },
    { "scrub@100", { 8700, 43500, false } },     { "scrub@10000", { 33000, 132000, true } },
};

// The duration-budget scale factor applied to the hard @10000 ceilings. The
// ceilings in kBudgets are calibrated on the dev host; a CI runner is ~10x
// slower, so the same ceiling would hard-fail every CI run on a healthy
// build. The scale relaxes the hard gates to absorb that environment without
// gutting the gate - an order-of-magnitude regression still trips on both
// hosts (see the headroom table in the block comment at the top of this file).
//
// Resolution order: an explicit GENESIS_PERF_BUDGET_SCALE wins; otherwise the
// `CI` env var (which GitHub Actions sets) implies 8x; otherwise 1.0. A
// non-positive override is a misconfiguration, not a scale - it is ignored
// rather than silently disabling every hard gate.
double budget_scale()
{
    const char *override = std::getenv("GENESIS_PERF_BUDGET_SCALE");
    if (override != nullptr && *override != '\0') {
        const double value = std::atof(override);
        if (value > 0.0) {
            return value;
        }
    }
    if (std::getenv("CI") != nullptr) {
        return 8.0;
    }
    return 1.0;
}

// Runs every benchmark at every size, printing the table and returning the
// results for the contract checks.
std::vector<genesis::perf::Result> run_all(double scale)
{
    std::vector<genesis::perf::Result> results;
    std::printf("duration budget scale: %.1fx\n", scale);
    std::printf("%-12s %8s %10s %10s %10s %8s  %s\n", "scenario", "size", "p50 us", "p95 us",
                "max us", "iters", "work");
    std::printf("---------------------------------------------------------------"
                "----\n");
    for (const genesis::perf::Benchmark &benchmark : genesis::perf::benchmarks()) {
        for (std::size_t size : kSizes) {
            const genesis::perf::Scenario scenario = benchmark.make(size);
            const genesis::perf::RunResult run =
                    genesis::perf::run_scenario(scenario, kWarmup, kIterations);
            genesis::perf::Result result{ benchmark.name, size, run.dist, run.work };
            results.push_back(result);
            std::printf("%-12s %8zu %10lld %10lld %10lld %8zu  %llu\n", benchmark.name.c_str(),
                        size, static_cast<long long>(run.dist.p50_us),
                        static_cast<long long>(run.dist.p95_us),
                        static_cast<long long>(run.dist.max_us), run.dist.iterations,
                        static_cast<unsigned long long>(run.work));
        }
    }
    return results;
}

} // namespace

int main(int argc, char **argv)
{
    // --check <baseline-file> [--threshold <pct>]: the CI gate. Without it,
    // the table plus the contract assertions.
    std::string check_path;
    double threshold = kThreshold;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--check" && i + 1 < argc) {
            check_path = argv[++i];
        } else if (arg == "--threshold" && i + 1 < argc) {
            threshold = std::atof(argv[++i]);
        }
    }

    const double scale = budget_scale();
    const std::vector<genesis::perf::Result> results = run_all(scale);

    if (!check_path.empty()) {
        const auto baseline = genesis::perf::read_baseline(check_path);
        std::string report;
        const bool holds = genesis::perf::check_against(results, baseline, threshold, report);
        std::printf("%s", report.c_str());
        std::printf(holds ? "every scenario within its threshold\n" : "regression detected\n");
        return holds ? 0 : 1;
    }

    // Every scenario ran and did work, with the configured iteration count.
    for (const genesis::perf::Result &result : results) {
        check(result.work > 0,
              "scenario " + result.name + "@" + std::to_string(result.size) + " did work");
        check(result.dist.iterations == kIterations,
              "scenario " + result.name + "@" + std::to_string(result.size)
                      + " ran the configured iterations");
    }

    // Duration budgets. A hard gate trips on a real regression; the
    // microsecond-scale sanity fixtures only warn. Every message shows the
    // measured p50 against the recorded baseline and the ceiling.
    for (const genesis::perf::Result &result : results) {
        const std::string key = genesis::perf::baseline_key(result);
        const auto budget_it = kBudgets.find(key);
        if (budget_it == kBudgets.end()) {
            check(false, key + " has no duration budget recorded");
            continue;
        }
        const Budget &budget = budget_it->second;
        // The scale relaxes only the hard @10000 gates. The @100 warn-only
        // ceilings never fail the run, so leaving them unscaled keeps their
        // WARN lines honest on a slow environment instead of hiding them.
        const std::int64_t ceiling = budget.hard_fail
                ? static_cast<std::int64_t>(budget.ceiling_us * scale)
                : budget.ceiling_us;
        const std::string message = key + ": p50 " + std::to_string(result.dist.p50_us)
                + "us vs budget " + std::to_string(ceiling) + "us (baseline "
                + std::to_string(budget.baseline_us) + "us)";
        if (budget.hard_fail) {
            check(result.dist.p50_us <= ceiling, message);
        } else if (result.dist.p50_us > ceiling) {
            std::printf("WARN: %s\n", message.c_str());
        }
    }

    // A zero-iteration config is refused.
    bool refused = false;
    try {
        const genesis::perf::Scenario scenario =
                genesis::perf::benchmarks().front().make(kSizes.front());
        (void)genesis::perf::run_scenario(scenario, 0, 0);
    } catch (const std::invalid_argument &) {
        refused = true;
    }
    check(refused, "a zero-iteration run is refused");

    // --check passes against the baseline just recorded, and fails against a
    // perturbed one: a baseline with an impossible budget (1 us) makes any
    // real p95 a regression, so the failure is deterministic rather than a
    // timing flake.
    const auto baseline = genesis::perf::baseline_of(results);
    {
        std::string report;
        check(genesis::perf::check_against(results, baseline, kThreshold, report),
              "--check passes against the just-recorded baseline");
    }
    {
        auto inflated = baseline;
        inflated[genesis::perf::baseline_key(results.front())].p95_us = 1;
        std::string report;
        check(!genesis::perf::check_against(results, inflated, kThreshold, report),
              "--check fails against an inflated baseline");
    }

    // The baseline file round-trips. Only the timing triple is persisted, so
    // the comparison is over p50/p95/max, not the (ephemeral) iteration count.
    {
        const std::string path =
                (std::filesystem::temp_directory_path() / "genesis-perf-baseline.txt").string();
        genesis::perf::write_baseline(path, results);
        const auto reread = genesis::perf::read_baseline(path);
        bool same = reread.size() == baseline.size();
        for (const auto &[key, dist] : baseline) {
            const auto it = reread.find(key);
            same = same && it != reread.end() && it->second.p50_us == dist.p50_us
                    && it->second.p95_us == dist.p95_us && it->second.max_us == dist.max_us;
        }
        check(same, "the baseline file round-trips");
    }

    return genesis::test::summary();
}
