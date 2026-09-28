// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The suites command: maps files to the ctest suites that compile or link them
// by parsing CMakeLists.txt's genesis_add_test registrations, so agents run
// the right `ctest -R` subset after an edit.

#include "tools/dev/commands.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "tools/dev/coupling.h"
#include "tools/dev/json.h"
#include "tools/dev/suite_parser.h"

namespace genesis::dev {

namespace {

std::string join(const std::vector<std::string> &names)
{
    std::string out;
    for (const std::string &name : names) {
        if (!out.empty()) {
            out += ", ";
        }
        out += name;
    }
    return out;
}

} // namespace

int suites_command(const SuitesArgs &args)
{
    std::ifstream in("CMakeLists.txt");
    if (!in) {
        std::fprintf(stderr, "suites: no CMakeLists.txt in the current directory\n");
        return 2;
    }
    const std::string text =
            std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());

    const std::vector<Suite> suites = parse_suites(text);

    if (args.files.empty()) {
        if (args.json) {
            Json json;
            json.begin_object();
            json.key("suites");
            json.begin_array();
            for (const Suite &suite : suites) {
                json.begin_object();
                json.key("name");
                json.string(suite.name);
                json.key("sources");
                json.begin_array();
                for (const std::string &source : suite.sources) {
                    json.string(source);
                }
                json.end_array();
                json.key("libraries");
                json.begin_array();
                for (const std::string &library : suite.libraries) {
                    json.string(library);
                }
                json.end_array();
                json.key("working_directory");
                if (suite.working_directory) {
                    json.string(*suite.working_directory);
                } else {
                    json.null();
                }
                json.key("gate");
                json.string(suite.gate);
                json.end_object();
            }
            json.end_array();
            json.end_object();
            std::printf("%s\n", json.take().c_str());
        } else {
            std::printf("%zu suites (sources, working directory and gate per suite)\n",
                        suites.size());
            std::printf("%-24s %3s %-6s %s\n", "suite", "src", "cwd", "gate");
            for (const Suite &suite : suites) {
                std::printf("%-24s %3zu %-6s %s\n", suite.name.c_str(), suite.sources.size(),
                            suite.working_directory ? "yes" : "-",
                            suite.gate.empty() ? "-" : suite.gate.c_str());
            }
        }
        return 0;
    }

    const std::vector<std::pair<std::string, std::string>> library_map = library_sources(text);

    if (args.json) {
        Json json;
        json.begin_object();
        json.key("files");
        json.begin_array();
        for (const std::string &file : args.files) {
            const FileCoupling coupling = couple_file(file, suites, library_map);
            json.begin_object();
            json.key("file");
            json.string(file);
            json.key("compiles");
            json.begin_array();
            for (const std::string &name : coupling.compiles) {
                json.string(name);
            }
            json.end_array();
            json.key("link_library");
            json.string(coupling.link_library);
            json.key("link_suites");
            json.begin_array();
            for (const std::string &name : coupling.link_suites) {
                json.string(name);
            }
            json.end_array();
            json.end_object();
        }
        json.end_array();
        json.end_object();
        std::printf("%s\n", json.take().c_str());
    } else {
        for (const std::string &file : args.files) {
            const FileCoupling coupling = couple_file(file, suites, library_map);
            std::printf("%s\n", file.c_str());
            if (!coupling.compiles.empty()) {
                std::printf("  compiles: %s\n", join(coupling.compiles).c_str());
            } else if (!coupling.link_library.empty()) {
                std::printf("  not a direct test source; compiled into %s, linked by %zu suites "
                            "(approximate): %s\n",
                            coupling.link_library.c_str(), coupling.link_suites.size(),
                            join(coupling.link_suites).c_str());
            } else {
                std::printf("  no suite compiles or links this file\n");
            }
        }
    }

    return 0;
}

} // namespace genesis::dev
