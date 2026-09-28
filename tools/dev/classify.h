// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Pure helpers for the smoke gate: the QML-runtime-message classifier, the
// /proc/<pid>/status RSS field parser, and the surface lists. No I/O here, so
// the dev_tools suite can exercise them without spawning the app.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace genesis::dev {

// True when a stderr line is a QML runtime message. Qt writes QML warnings and
// errors as `<file>:<line>: <message>`, where resource files use the `qrc:/`
// prefix and source-tree files use `file:///`. Leading whitespace is skipped.
// These lines are failures in the smoke gate.
bool is_qml_message(std::string_view line);

// True when a stderr line is the app's own informational log - the `[app]`
// prefix app_log callers use. Reported for context, never a failure.
bool is_app_log(std::string_view line);

// Parses one /proc/<pid>/status field of the form `Key:<whitespace><number> kB`
// and returns the number. Returns nullopt for any other line or key.
std::optional<long long> parse_status_kib(std::string_view line, std::string_view key);

// The surfaces the app's `--screenshot-open` switch understands, discovered
// from Main.qml's runAction() switch (the panel/dialog surface cases) plus the
// bare `shell` case (the shell window with no dialog). `--surfaces` validates
// against this list.
const std::vector<std::string> &known_surfaces();

// The default smoke set: the six surfaces worth gating on every UI change.
const std::vector<std::string> &default_surfaces();

} // namespace genesis::dev
