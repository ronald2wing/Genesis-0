// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Textual parser for the repo's `genesis_add_test(NAME … SOURCES … LIBRARIES …
// [WORKING_DIRECTORY …])` registrations in CMakeLists.txt. The `suites` command
// uses it to map changed files to the ctest suites that compile or link them,
// so agents run the right `ctest -R` subset. It is a parse of the registration
// text, not a CMake evaluation: comments are stripped, multi-line blocks are
// matched by balanced parentheses, and the enclosing `if(<cond>)` condition is
// recorded as each suite's gate (the Qt-gated block being the notable one).

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace genesis::dev {

// One genesis_add_test(...) registration.
struct Suite
{
    std::string name;
    std::vector<std::string> sources; // SOURCES entries, in order
    std::vector<std::string> libraries; // LIBRARIES entries, in order
    std::optional<std::string> working_directory; // WORKING_DIRECTORY, if present
    std::string gate; // innermost if(<cond>), or empty
};

// Parses every genesis_add_test(...) registration in `text`.
std::vector<Suite> parse_suites(std::string_view text);

// Maps each source file compiled into one of the three genesis libraries
// (genesis_host, genesis_ai, genesis_engine_ges) to that library's name, so the
// suites command can report "links <lib>" for a file no suite compiles
// directly. Pairs are (library, source file), in registration order.
std::vector<std::pair<std::string, std::string>> library_sources(std::string_view text);

} // namespace genesis::dev
