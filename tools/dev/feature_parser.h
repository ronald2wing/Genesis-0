// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Textual parsers for the repo's feature surfaces, so `genesis-dev features`
// and `genesis-dev where` can list what the product does without a
// hand-maintained manifest that drifts. Each parser reads one source of truth:
//
//   - API verbs      src/api/Requests.cpp   `return "verb.name";`
//   - CLI verbs      src/cli/main.cpp       `add_subcommand("name", "desc")`
//   - command verbs  src/cli/Commands.cpp   the `{ "Name", "desc" }` table
//   - QML surfaces   src/app/qml/*.qml      one per file
//   - controllers    src/app/controllers/*.h one per file
//
// These are parses of the source text, not a C++ evaluation: a verb is a string
// literal on a `return` line, a subcommand is an `add_subcommand` call. The
// dev_tools suite pins the counts so a new verb without a parser update fails
// loudly rather than silently dropping from the index.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace genesis::dev {

// One feature surface, as discovered from the tree.
struct Feature
{
    std::string name; // "edit.apply", "render", "TimelineStrip"
    std::string kind; // "api-verb", "cli-verb", "command-verb", "qml-surface", "controller"
    std::string source; // "src/api/Requests.cpp:33" (file:line, or just file)
    std::string description; // from the source where available, else empty
};

// Scans `text` (the contents of src/api/Requests.cpp) for `return "…";` string
// literals and returns one api-verb Feature per literal, with its 1-based line.
std::vector<Feature> parse_api_verbs(std::string_view text);

// Scans `text` (src/cli/main.cpp) for `add_subcommand("name", "desc")` and
// `add_subcommand("name")` calls, returning one cli-verb Feature per call.
std::vector<Feature> parse_cli_verbs(std::string_view text);

// Scans `text` (src/cli/Commands.cpp) for the `{ "Name", "description" }` verb
// table and returns one command-verb Feature per entry.
std::vector<Feature> parse_command_verbs(std::string_view text);

// Lists `dir` for `*.qml` files, one qml-surface Feature per file (name is the
// basename without extension). Returns empty when the directory is unreadable.
std::vector<Feature> parse_qml_surfaces(const std::string &dir);

// Lists `dir` for `*.h` files, one controller Feature per file (name is the
// basename without extension). Returns empty when the directory is unreadable.
std::vector<Feature> parse_controllers(const std::string &dir);

} // namespace genesis::dev
