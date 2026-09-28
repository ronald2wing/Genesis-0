// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Argument parsing for the three genesis-dev subcommands. Pure (no I/O): each
// parse function takes the subcommand's argument vector (without the program
// name or subcommand) and returns a struct whose `ok`/`error` fields report the
// first problem. The dev_tools suite exercises these directly.

#pragma once

#include <string>
#include <vector>

namespace genesis::dev {

struct SmokeArgs
{
    bool ok = true;
    std::string error;
    bool json = false;

    std::string app = "build/bin/genesis-app";
    std::string out = "build/ai-smoke";
    long seconds_ms = 8000; // per-surface capture deadline
    long long max_rss_mb = 2048;
    std::vector<std::string> surfaces; // empty -> default set
};

struct VerifyArgs
{
    bool ok = true;
    std::string error;
    bool json = false;

    bool skip_smoke = false;
    bool strict = false;
    std::string build_dir = "build";
};

struct SuitesArgs
{
    bool ok = true;
    std::string error;
    bool json = false;

    std::vector<std::string> files;
};

struct FeaturesArgs
{
    bool ok = true;
    std::string error;
    bool json = false;
};

struct WhereArgs
{
    bool ok = true;
    std::string error;
    bool json = false;

    std::vector<std::string> keywords;
};

SmokeArgs parse_smoke_args(const std::vector<std::string> &argv);
VerifyArgs parse_verify_args(const std::vector<std::string> &argv);
SuitesArgs parse_suites_args(const std::vector<std::string> &argv);
FeaturesArgs parse_features_args(const std::vector<std::string> &argv);
WhereArgs parse_where_args(const std::vector<std::string> &argv);

} // namespace genesis::dev
