// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The benchmark driver: runs the measurable scenarios, prints the table, and
// asserts the *contract* of the harness - that every scenario ran and did
// work, that a zero-iteration run is refused, and that --check passes against
// a just-recorded baseline and fails against a perturbed one. No duration is
// ever asserted: a timing assertion is flaky on a shared runner, so the
// assertions are about counts and the check's logic, not wall-clock numbers.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "perf/Benchmarks.h"

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

// The sizes a scenario must be meaningful at, and the fixed warm-up and
// iteration counts. -O2 is assumed: a -O0 benchmark measures nothing.
const std::vector<std::size_t> kSizes = { 100, 10000 };
const std::size_t kWarmup = 2;
const std::size_t kIterations = 20;
const double kThreshold = 25.0;

// Runs every benchmark at every size, printing the table and returning the
// results for the contract checks.
std::vector<genesis::perf::Result> run_all()
{
    std::vector<genesis::perf::Result> results;
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

    const std::vector<genesis::perf::Result> results = run_all();

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

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
