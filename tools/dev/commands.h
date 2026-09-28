// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The genesis-dev command surface: one entry function per subcommand plus the
// smoke gate's report types (verify folds a smoke run's result into its own
// summary, so the core is exposed here).

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "tools/dev/args.h"

namespace genesis::dev {

// One surface's smoke outcome.
struct SurfaceResult
{
    std::string surface;
    bool spawned = false;
    bool timed_out = false;
    int exit_code = -1;
    long long wall_ms = 0;
    bool png = false;
    long long png_bytes = 0;
    long long peak_rss_kb = 0;
    std::vector<std::string> qml_lines; // QML runtime messages (failures)
    int app_log_lines = 0; // informational [app] lines
    std::vector<std::pair<long long, long long>> rss_trajectory; // (t_ms, rss_kb)
};

struct SmokeReport
{
    SmokeArgs args;
    std::vector<SurfaceResult> surfaces;
    bool fixture_ready = true; // the demo media fixture exists (or was generated)
    int passed = 0;
    int failed = 0;
};

// Runs the smoke gate end to end: spawns the app per surface, samples its RSS,
// classifies its stderr, and fills the report. Does not print or write files.
SmokeReport run_smoke(const SmokeArgs &args);

// Renders `report` (human or --json), writes report.json, and returns the exit
// code (0 all surfaces clean, 1 any failure, 2 usage error).
int smoke_command(const SmokeArgs &args);

int verify_command(const VerifyArgs &args);
int suites_command(const SuitesArgs &args);
int features_command(const FeaturesArgs &args);
int where_command(const WhereArgs &args);

// The top-level usage text for `--help` or an unknown subcommand.
const char *usage_text();

} // namespace genesis::dev
