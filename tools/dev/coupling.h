// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The file -> suite coupling shared by the `suites` and `where` commands: given
// a source file, which ctest suites compile it directly, or (when none do)
// which genesis library compiles it and which suites link that library. Pure
// (no I/O), so the dev_tools suite exercises it directly.

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "tools/dev/suite_parser.h"

namespace genesis::dev {

// One changed file's coupling to the suites.
struct FileCoupling
{
    std::string file;
    std::vector<std::string> compiles; // suites that list it in SOURCES
    std::string link_library; // a genesis library that compiles it
    std::vector<std::string> link_suites; // suites linking that library
};

// Strips a leading `./` and any absolute prefix under the current directory,
// yielding the repo-relative form sources are listed in.
std::string repo_relative(const std::string &path);

// Resolves `file` against `suites` and `library_map` (as returned by
// `library_sources`). A direct SOURCES match wins; otherwise the file's
// compiling library and the suites that link it are reported.
FileCoupling couple_file(const std::string &file, const std::vector<Suite> &suites,
                         const std::vector<std::pair<std::string, std::string>> &library_map);

} // namespace genesis::dev
