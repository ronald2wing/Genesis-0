// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The app's one diagnostic sink: every message goes to stderr - the format the
// app already spoke - and, when a run log is installed, to that file too. The
// composition root opens the LogFile and installs it here; a null file (the
// open failed) leaves stderr as the only sink, so logging never breaks startup.
//
// Levels gate what is written: `error` < `warn` < `info` < `debug`, and a
// message is written when its level does not exceed the current one (Info by
// default). GENESIS_LOG in the environment, or the --log-level flag, raises or
// lowers that ceiling.

#pragma once

#include <optional>
#include <string_view>

// The printf-format attribute is GCC/Clang-only; the strict warning set that
// would use it is applied only to those compilers (see genesis_warnings).
#if defined(__GNUC__) || defined(__clang__)
#  define GENESIS_PRINTF_FORMAT(fmt, args) __attribute__((format(printf, fmt, args)))
#else
#  define GENESIS_PRINTF_FORMAT(fmt, args)
#endif

namespace genesis::workspace {
class LogFile;
}

namespace genesis::app {

// Severity levels, ascending: a message at `level` is written when `level`
// does not exceed the current level (see set_log_level).
enum class LogLevel { Error, Warn, Info, Debug };

// Parses a level name - "error", "warn", "info" or "debug" in any case, with
// surrounding whitespace - into a level. Returns nullopt for any other name
// (including empty).
std::optional<LogLevel> parse_log_level(std::string_view name);

// The lower-case name parse_log_level reads back.
const char *log_level_name(LogLevel level) noexcept;

// Sets the level at or below which messages are written. Defaults to Info.
void set_log_level(LogLevel level);

// The current level.
LogLevel log_level() noexcept;

// Applies the level `name` names: a recognised name sets it, anything else is
// reported (warn) and the level becomes Info. Returns the applied level.
LogLevel apply_log_level(std::string_view name);

// Reads GENESIS_LOG once (the first call only) and applies it; an absent or
// empty variable leaves the level as it is. Returns the applied level.
LogLevel apply_environment_log_level();

// Installs the run log `app_log` writes to, without taking ownership. Pass
// nullptr to detach before the file is destroyed. Single-threaded by contract:
// the app logs from the UI thread only.
void set_log_file(genesis::workspace::LogFile *log);

// Writes one printf-formatted message at Info level.
void app_log(const char *format, ...) GENESIS_PRINTF_FORMAT(1, 2);

// Writes one printf-formatted message at `level`. Messages below the current
// level are dropped before formatting.
void app_log_at(LogLevel level, const char *format, ...) GENESIS_PRINTF_FORMAT(2, 3);

} // namespace genesis::app
