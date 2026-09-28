// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/support/AppLog.h"

#include <cctype>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include "app/support/CrashHandler.h"
#include "workspace/Logs.h"

namespace genesis::app {

namespace {

genesis::workspace::LogFile *g_log = nullptr;
LogLevel g_level = LogLevel::Info;

// A message at `level` is written when it does not exceed the current level.
bool should_write(LogLevel level) noexcept
{
    return static_cast<int>(level) <= static_cast<int>(g_level);
}

// The run-log prefix a non-Info line carries, so a grep for [error] finds only
// genuine problems. Info lines carry none - the format callers always wrote.
const char *tag(LogLevel level) noexcept
{
    switch (level) {
    case LogLevel::Error:
        return "[error] ";
    case LogLevel::Warn:
        return "[warn] ";
    case LogLevel::Debug:
        return "[debug] ";
    case LogLevel::Info:
        return "";
    }
    return "";
}

// The core write: hands the same bytes to stderr and, when a run log is
// installed, to it too. Only the run-log copy carries the level tag; stderr
// keeps the exact text callers always printed.
void write_message(LogLevel level, std::string_view message)
{
    std::fputs(message.data(), stderr);

    if (g_log != nullptr) {
        while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
            message.remove_suffix(1);
        }
        const char *prefix = tag(level);
        if (*prefix == '\0') {
            g_log->log(message);
        } else {
            std::string tagged;
            tagged.reserve(message.size() + std::strlen(prefix));
            tagged.append(prefix);
            tagged.append(message);
            g_log->log(tagged);
        }
    }
}

// Formats one message at `level` and writes it. The va_list has been started by
// the caller, which also ends it.
void emit(LogLevel level, const char *format, std::va_list *args)
{
    if (!should_write(level)) {
        return;
    }
    std::va_list measured;
    va_copy(measured, *args);
    const int size = std::vsnprintf(nullptr, 0, format, measured);
    va_end(measured);
    if (size < 0) {
        return;
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    std::vsnprintf(text.data(), text.size() + 1, format, *args);
    write_message(level, text);
}

} // namespace

std::optional<LogLevel> parse_log_level(std::string_view name)
{
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front())) != 0) {
        name.remove_prefix(1);
    }
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())) != 0) {
        name.remove_suffix(1);
    }
    const auto lower = [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    };
    const auto equals = [&](std::string_view candidate) {
        if (name.size() != candidate.size()) {
            return false;
        }
        for (std::size_t index = 0; index < name.size(); ++index) {
            if (lower(name[index]) != candidate[index]) {
                return false;
            }
        }
        return true;
    };
    if (equals("error")) {
        return LogLevel::Error;
    }
    if (equals("warn")) {
        return LogLevel::Warn;
    }
    if (equals("info")) {
        return LogLevel::Info;
    }
    if (equals("debug")) {
        return LogLevel::Debug;
    }
    return std::nullopt;
}

const char *log_level_name(LogLevel level) noexcept
{
    switch (level) {
    case LogLevel::Error:
        return "error";
    case LogLevel::Warn:
        return "warn";
    case LogLevel::Info:
        return "info";
    case LogLevel::Debug:
        return "debug";
    }
    return "info";
}

void set_log_level(LogLevel level)
{
    g_level = level;
}

LogLevel log_level() noexcept
{
    return g_level;
}

LogLevel apply_log_level(std::string_view name)
{
    if (const std::optional<LogLevel> parsed = parse_log_level(name)) {
        set_log_level(*parsed);
        const std::string announcement =
                std::string("log level: ") + log_level_name(*parsed) + "\n";
        write_message(LogLevel::Info, announcement);
        return *parsed;
    }
    set_log_level(LogLevel::Info);
    std::string message = "ignoring invalid log level \"";
    message.append(name);
    message.append("\"; using info\n");
    write_message(LogLevel::Warn, message);
    return LogLevel::Info;
}

LogLevel apply_environment_log_level()
{
    static bool read = false;
    if (read) {
        return log_level();
    }
    read = true;
    const char *value = std::getenv("GENESIS_LOG");
    if (value == nullptr || *value == '\0') {
        return log_level();
    }
    return apply_log_level(value);
}

void set_log_file(genesis::workspace::LogFile *log)
{
    g_log = log;
    set_crash_log_path(log != nullptr ? log->path().string().c_str() : nullptr);
}

void app_log(const char *format, ...)
{
    std::va_list args;
    va_start(args, format);
    emit(LogLevel::Info, format, &args);
    va_end(args);
}

void app_log_at(LogLevel level, const char *format, ...)
{
    std::va_list args;
    va_start(args, format);
    emit(level, format, &args);
    va_end(args);
}

} // namespace genesis::app
