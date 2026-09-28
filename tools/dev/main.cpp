// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// genesis-dev: the Genesis-0 AI-agent/dev workflow tool. Three subcommands
// (smoke, verify, suites) share one entry point; each prints a concise human
// report or, with --json, a machine-readable document for agents.

#include <cstdio>
#include <string>
#include <vector>

#include "tools/dev/args.h"
#include "tools/dev/commands.h"

namespace genesis::dev {

const char *usage_text()
{
    return "genesis-dev - the Genesis-0 AI-agent/dev workflow tool\n"
           "\n"
           "usage: genesis-dev <command> [options]\n"
           "\n"
           "commands:\n"
           "  smoke [--app <path>] [--out <dir>] [--seconds <ms>] [--surfaces a,b,...]\n"
           "        [--max-rss-mb <n>] [--json]\n"
           "      Spawn the app offscreen (software GL), screenshot each surface, sample\n"
           "      its RSS, and flag QML runtime messages. Writes <out>/report.json.\n"
           "      Exits non-zero on any surface failure.\n"
           "      Surfaces: shell,settings,extensions,export,titles,scopes (default),\n"
           "      plus cutout,enhance,keyframes.\n"
           "\n"
           "  verify [--skip-smoke] [--build-dir <dir>] [--strict] [--json]\n"
           "      Configure (if needed), build, run ctest, re-run each failure in\n"
           "      isolation to classify flake vs real, then fold in the smoke gate.\n"
           "\n"
           "  suites [<file>...] [--json]\n"
           "      Parse CMakeLists.txt genesis_add_test registrations. No args: list\n"
           "      suites (sources + cwd + gate). With files: report the suites that\n"
           "      compile (or link) each file.\n"
           "\n"
           "  features [--json]\n"
           "      List every feature surface parsed live from the tree: API verbs,\n"
           "      CLI verbs, command verbs, QML surfaces and controllers. The index\n"
           "      cannot drift from the code because it is read from the code.\n"
           "\n"
           "  where <keyword>... [--json]\n"
           "      The debug loop: map a symptom word to the matching features, the\n"
           "      ctest suites that pin them, the smoke surface that exercises them,\n"
           "      and the exact command to reproduce.\n";
}

} // namespace genesis::dev

int main(int argc, char *argv[])
{
    const std::vector<std::string> args(argv + 1, argv + argc);

    if (args.empty()) {
        std::fprintf(stderr, "%s", genesis::dev::usage_text());
        return 2;
    }
    if (args[0] == "--help" || args[0] == "-h" || args[0] == "help") {
        std::printf("%s", genesis::dev::usage_text());
        return 0;
    }

    const std::string command = args[0];
    const std::vector<std::string> rest(args.begin() + 1, args.end());

    if (command == "smoke") {
        genesis::dev::SmokeArgs parsed = genesis::dev::parse_smoke_args(rest);
        if (!parsed.ok) {
            std::fprintf(stderr, "smoke: %s\n", parsed.error.c_str());
            return 2;
        }
        return genesis::dev::smoke_command(parsed);
    }
    if (command == "verify") {
        genesis::dev::VerifyArgs parsed = genesis::dev::parse_verify_args(rest);
        if (!parsed.ok) {
            std::fprintf(stderr, "verify: %s\n", parsed.error.c_str());
            return 2;
        }
        return genesis::dev::verify_command(parsed);
    }
    if (command == "suites") {
        genesis::dev::SuitesArgs parsed = genesis::dev::parse_suites_args(rest);
        if (!parsed.ok) {
            std::fprintf(stderr, "suites: %s\n", parsed.error.c_str());
            return 2;
        }
        return genesis::dev::suites_command(parsed);
    }
    if (command == "features") {
        genesis::dev::FeaturesArgs parsed = genesis::dev::parse_features_args(rest);
        if (!parsed.ok) {
            std::fprintf(stderr, "features: %s\n", parsed.error.c_str());
            return 2;
        }
        return genesis::dev::features_command(parsed);
    }
    if (command == "where") {
        genesis::dev::WhereArgs parsed = genesis::dev::parse_where_args(rest);
        if (!parsed.ok) {
            std::fprintf(stderr, "where: %s\n", parsed.error.c_str());
            return 2;
        }
        return genesis::dev::where_command(parsed);
    }

    std::fprintf(stderr, "genesis-dev: unknown command \"%s\"\n\n%s", command.c_str(),
                 genesis::dev::usage_text());
    return 2;
}
