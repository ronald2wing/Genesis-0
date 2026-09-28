// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace genesis::workspace {

// How many run logs a folder keeps: the newest KEEP survive and older ones
// are removed when a new run starts.
inline constexpr std::size_t KEEP = 10;
// Bytes a run may write before the log gives up: 16 MiB.
inline constexpr std::uint64_t CAP = 16ULL * 1024 * 1024;

// One run's log file, appended to as the app works.
//
// The file is named `genesis-YYYYMMDD-HHMMSS.log` in the caller's directory,
// which the opener creates if it does not exist. Each `log` line carries a UTC
// timestamp. The file is bounded: once a run has written CAP bytes the rest is
// dropped after a one-line notice, so a runaway loop cannot fill the disk.
// Only `std::ofstream` and `std::chrono` are used - no logger library.
class LogFile
{
public:
    LogFile(LogFile &&) = default;
    LogFile &operator=(LogFile &&) = default;
    LogFile(const LogFile &) = delete;
    LogFile &operator=(const LogFile &) = delete;

    // Opens the next log file for a run: prunes the folder down to KEEP - 1
    // survivors, then appends to a freshly stamped file. Returns nullopt only
    // when the directory cannot be made or the file cannot be opened.
    static std::optional<LogFile> open(const std::filesystem::path &dir, std::uint64_t cap = CAP);

    ~LogFile() { close(); }

    // Appends one timestamped line. Once the file reaches its cap this writes
    // a single notice and then nothing further.
    void log(std::string_view message);

    // Flushes and closes the file. Idempotent.
    void close();

    const std::filesystem::path &path() const noexcept { return path_; }

private:
    LogFile() = default;

    std::filesystem::path path_;
    std::ofstream stream_;
    std::uint64_t cap_ = CAP;
    std::uint64_t written_ = 0;
    bool capped_ = false;
};

} // namespace genesis::workspace
