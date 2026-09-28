// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "tools/dev/feature_parser.h"

#include <algorithm>
#include <filesystem>

namespace genesis::dev {

namespace {

// The 1-based line number of `offset` within `text`.
int line_of(std::string_view text, std::size_t offset)
{
    int line = 1;
    for (std::size_t i = 0; i < offset && i < text.size(); ++i) {
        if (text[i] == '\n') {
            ++line;
        }
    }
    return line;
}

// Extracts the first double-quoted string starting at or after `from`, and
// advances `end` past its closing quote. Returns false when none is found.
bool next_quoted(std::string_view text, std::size_t from, std::string &out, std::size_t &end)
{
    const std::size_t open = text.find('"', from);
    if (open == std::string_view::npos) {
        return false;
    }
    const std::size_t close = text.find('"', open + 1);
    if (close == std::string_view::npos) {
        return false;
    }
    out = std::string(text.substr(open + 1, close - open - 1));
    end = close + 1;
    return true;
}

// Lists `dir` for files with `extension`, sorted by name, returning the
// basename without the extension. Empty when the directory is unreadable.
std::vector<std::string> list_stems(const std::string &dir, std::string_view extension)
{
    std::vector<std::string> stems;
    std::error_code ec;
    for (const std::filesystem::directory_entry &entry :
         std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (name.size() > extension.size()
            && name.compare(name.size() - extension.size(), extension.size(), extension) == 0) {
            stems.push_back(name.substr(0, name.size() - extension.size()));
        }
    }
    std::sort(stems.begin(), stems.end());
    return stems;
}

} // namespace

std::vector<Feature> parse_api_verbs(std::string_view text)
{
    std::vector<Feature> features;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t ret = text.find("return \"", pos);
        if (ret == std::string_view::npos) {
            break;
        }
        std::string name;
        std::size_t end = 0;
        if (!next_quoted(text, ret + 7, name, end)) {
            break;
        }
        // A method name is a bare identifier or a dotted pair; skip anything
        // else (a stray string in a comment or a non-method return).
        const bool valid = !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.';
        });
        if (valid) {
            features.push_back(
                    Feature{ name, "api-verb",
                             "src/api/Requests.cpp:" + std::to_string(line_of(text, ret)), "" });
        }
        pos = end;
    }
    return features;
}

std::vector<Feature> parse_cli_verbs(std::string_view text)
{
    std::vector<Feature> features;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t call = text.find("add_subcommand(\"", pos);
        if (call == std::string_view::npos) {
            break;
        }
        std::string name;
        std::size_t end = 0;
        if (!next_quoted(text, call + 15, name, end)) {
            break;
        }
        // The description is the next quoted string, when the call carries one.
        std::string description;
        std::size_t desc_end = 0;
        const std::size_t comma = text.find(',', end);
        const std::size_t close = text.find(')', end);
        if (comma != std::string_view::npos && (close == std::string_view::npos || comma < close)) {
            next_quoted(text, comma, description, desc_end);
        }
        features.push_back(Feature{ name, "cli-verb",
                                    "src/cli/main.cpp:" + std::to_string(line_of(text, call)),
                                    description });
        pos = end;
    }
    return features;
}

std::vector<Feature> parse_command_verbs(std::string_view text)
{
    std::vector<Feature> features;
    // The verb table is the `{ "Name", "description" }` initializer list inside
    // cmd_commands. Anchor on the function, then read each brace pair.
    const std::size_t fn = text.find("cmd_commands");
    if (fn == std::string_view::npos) {
        return features;
    }
    std::size_t pos = fn;
    while (pos < text.size()) {
        const std::size_t open = text.find("{ \"", pos);
        if (open == std::string_view::npos) {
            break;
        }
        std::string name;
        std::size_t end = 0;
        if (!next_quoted(text, open + 2, name, end)) {
            break;
        }
        std::string description;
        std::size_t desc_end = 0;
        const std::size_t comma = text.find(',', end);
        const std::size_t close = text.find('}', end);
        if (comma != std::string_view::npos && (close == std::string_view::npos || comma < close)) {
            next_quoted(text, comma, description, desc_end);
        }
        features.push_back(Feature{ name, "command-verb",
                                    "src/cli/Commands.cpp:" + std::to_string(line_of(text, open)),
                                    description });
        pos = end;
    }
    return features;
}

std::vector<Feature> parse_qml_surfaces(const std::string &dir)
{
    std::vector<Feature> features;
    for (const std::string &stem : list_stems(dir, ".qml")) {
        features.push_back(Feature{ stem, "qml-surface", dir + "/" + stem + ".qml", "" });
    }
    return features;
}

std::vector<Feature> parse_controllers(const std::string &dir)
{
    std::vector<Feature> features;
    for (const std::string &stem : list_stems(dir, ".h")) {
        features.push_back(Feature{ stem, "controller", dir + "/" + stem + ".h", "" });
    }
    return features;
}

} // namespace genesis::dev
