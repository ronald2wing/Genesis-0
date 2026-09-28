// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "tools/dev/classify.h"

#include <cctype>

namespace genesis::dev {

namespace {

std::string_view trim_leading(std::string_view line)
{
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())) != 0) {
        line.remove_prefix(1);
    }
    return line;
}

bool starts_with(std::string_view line, std::string_view prefix)
{
    return line.size() >= prefix.size() && line.substr(0, prefix.size()) == prefix;
}

} // namespace

bool is_qml_message(std::string_view line)
{
    const std::string_view trimmed = trim_leading(line);
    return starts_with(trimmed, "qrc:/") || starts_with(trimmed, "file:///");
}

bool is_app_log(std::string_view line)
{
    return line.find("[app]") != std::string_view::npos;
}

std::optional<long long> parse_status_kib(std::string_view line, std::string_view key)
{
    if (!starts_with(line, key)) {
        return std::nullopt;
    }
    std::string_view rest = line.substr(key.size());
    if (!rest.empty() && rest.front() == ':') {
        rest.remove_prefix(1);
    }
    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) {
        rest.remove_prefix(1);
    }
    long long value = 0;
    bool any = false;
    while (!rest.empty() && rest.front() >= '0' && rest.front() <= '9') {
        value = value * 10 + (rest.front() - '0');
        rest.remove_prefix(1);
        any = true;
    }
    return any ? std::optional<long long>(value) : std::nullopt;
}

const std::vector<std::string> &known_surfaces()
{
    static const std::vector<std::string> surfaces = { "shell",     "settings", "extensions",
                                                       "export",    "titles",   "scopes",
                                                       "cutout",    "enhance",  "keyframes",
                                                       "newproject" };
    return surfaces;
}

const std::vector<std::string> &default_surfaces()
{
    // Every known surface runs by default: the gate is the "review all
    // features" check, so a surface that regresses must fail it, not sit
    // outside it. `known_surfaces` is the single source of truth.
    return known_surfaces();
}

} // namespace genesis::dev
