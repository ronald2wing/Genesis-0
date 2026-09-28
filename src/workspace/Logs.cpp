// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "workspace/Logs.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace genesis::workspace {

namespace {

// The two spellings of the same instant: `File` names a file so its stamp
// sorts chronologically, `Line` prefixes a line for a person reading it.
enum class Stamp { File, Line };

// A UTC stamp straight from the wall clock's time_point, no tzdb, no locale:
// the calendar arithmetic is the standard library's, so the digits are always
// ASCII and always zero-padded.
std::string stamp(const std::chrono::system_clock::time_point &now, Stamp kind)
{
    const auto days = std::chrono::floor<std::chrono::days>(now);
    const std::chrono::year_month_day calendar{ days };
    const std::chrono::hh_mm_ss tod{ std::chrono::floor<std::chrono::milliseconds>(now - days) };

    const int year = static_cast<int>(calendar.year());
    const unsigned month = static_cast<unsigned>(calendar.month());
    const unsigned day = static_cast<unsigned>(calendar.day());
    const int hours = static_cast<int>(tod.hours().count());
    const int minutes = static_cast<int>(tod.minutes().count());
    const int seconds = static_cast<int>(tod.seconds().count());
    const int millis = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(tod.subseconds()).count());

    char buffer[32];
    if (kind == Stamp::File) {
        std::snprintf(buffer, sizeof buffer, "%04d%02u%02u-%02d%02d%02d", year, month, day, hours,
                      minutes, seconds);
    } else {
        std::snprintf(buffer, sizeof buffer, "%04d-%02u-%02u %02d:%02d:%02d.%03dZ", year, month,
                      day, hours, minutes, seconds, millis);
    }
    return std::string(buffer);
}

// Drops the oldest run logs until KEEP - 1 remain, so the file about to be
// created has room. Sorted by name - the file stamp sorts chronologically -
// so the oldest go first. Errors are ignored: pruning is housekeeping, and a
// log that cannot be deleted is no reason to refuse to open the next one.
void prune(const std::filesystem::path &dir)
{
    std::vector<std::filesystem::path> logs;
    std::error_code error;
    for (std::filesystem::directory_iterator it(dir, error), end; !error && it != end;
         it.increment(error)) {
        const std::string name = it->path().filename().string();
        if (name.rfind("genesis-", 0) == 0 && name.ends_with(".log")) {
            logs.push_back(it->path());
        }
    }
    std::sort(logs.begin(), logs.end());
    if (logs.size() <= KEEP - 1) {
        return;
    }
    for (std::size_t index = 0; index < logs.size() - (KEEP - 1); ++index) {
        std::error_code remove_error;
        std::filesystem::remove(logs[index], remove_error);
    }
}

} // namespace

std::optional<LogFile> LogFile::open(const std::filesystem::path &dir, std::uint64_t cap)
{
    // The directory is created rather than refused: the log is the only way a
    // headless run reports itself, and a missing directory should not take
    // that away.
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    if (error) {
        return std::nullopt;
    }
    prune(dir);

    LogFile file;
    file.path_ = dir / ("genesis-" + stamp(std::chrono::system_clock::now(), Stamp::File) + ".log");
    file.cap_ = cap;
    file.stream_.open(file.path_, std::ios::app);
    if (!file.stream_.is_open()) {
        return std::nullopt;
    }
    return file;
}

void LogFile::log(std::string_view message)
{
    if (!stream_.is_open() || capped_) {
        return;
    }
    if (written_ >= cap_) {
        // One notice, then silence for the rest of the run. The notice is
        // written even though the cap is passed - the reader needs to know
        // why the log stops, and a run that has already blown its cap will
        // not write enough more to matter.
        capped_ = true;
        const std::string notice = stamp(std::chrono::system_clock::now(), Stamp::Line)
                + " this run passed " + std::to_string(cap_)
                + " bytes of log; the rest is not written down\n";
        stream_ << notice;
        stream_.flush();
        written_ += notice.size();
        return;
    }
    const std::string line = stamp(std::chrono::system_clock::now(), Stamp::Line) + " "
            + std::string(message) + "\n";
    stream_ << line;
    stream_.flush();
    written_ += line.size();
}

void LogFile::close()
{
    if (stream_.is_open()) {
        stream_.flush();
        stream_.close();
    }
}

} // namespace genesis::workspace
