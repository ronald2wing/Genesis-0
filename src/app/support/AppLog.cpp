// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/support/AppLog.h"

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

#include "workspace/Logs.h"

namespace genesis::app {

namespace {
// The installed run log, or null when the open failed and stderr is the only
// sink. Owned by the composition root, which outlives every caller.
genesis::workspace::LogFile *g_log = nullptr;
} // namespace

void set_log_file(genesis::workspace::LogFile *log)
{
    g_log = log;
}

void app_log(const char *format, ...)
{
    std::va_list args;
    va_start(args, format);
    std::va_list measured;
    va_copy(measured, args);
    const int size = std::vsnprintf(nullptr, 0, format, measured);
    va_end(measured);
    if (size < 0) {
        va_end(args);
        return;
    }

    // Format once, then write the same bytes to both sinks: stderr keeps the
    // exact text callers printed before, and the run log gets it (minus the
    // trailing newline its own timestamped line adds).
    std::string text(static_cast<std::size_t>(size), '\0');
    std::vsnprintf(text.data(), text.size() + 1, format, args);
    va_end(args);
    std::fputs(text.c_str(), stderr);

    if (g_log != nullptr) {
        std::string_view line(text);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.remove_suffix(1);
        }
        g_log->log(line);
    }
}

} // namespace genesis::app
