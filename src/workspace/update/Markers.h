// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace genesis::workspace::update {

// The marker files the update store keeps under `root`, each a single line.
// These names are an on-disk contract shared by the install pipeline
// (Updater.cpp) and the apply path (Apply.cpp): a reader on either side must
// see the same bytes.
inline constexpr const char *kCurrent = "current";
inline constexpr const char *kPending = "pending";
inline constexpr const char *kConfirmed = "confirmed";
// The newest version that has run, the anti-downgrade/replay high-water mark:
// a manifest at or below it is refused. Advanced by confirm(); seeded from the
// running version on a fresh install (docs/decisions/auto-update.md §10).
inline constexpr const char *kLastSeen = "last-seen-version";
// The absolute path of the `target.old-<ts>` backup an apply left behind, so
// rollback() knows exactly what to restore without globbing the install root.
inline constexpr const char *kBackup = "backup";

// Reads a marker file's single line (newline and trailing whitespace trimmed).
// Absent or unreadable -> nullopt.
inline std::optional<std::string> read_marker(const std::filesystem::path &path)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return std::nullopt;
    }
    std::ifstream in(path);
    std::string value;
    std::getline(in, value);
    while (!value.empty()
           && (value.back() == '\n' || value.back() == '\r' || value.back() == ' '
               || value.back() == '\t')) {
        value.pop_back();
    }
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

// Writes a marker by writing a sibling then renaming over the target, so a
// reader never observes a half-written marker (atomic on the same filesystem).
inline bool write_marker(const std::filesystem::path &path, std::string_view value)
{
    const std::filesystem::path tmp = std::filesystem::path(path.string() + ".tmp");
    std::error_code ec;
    std::ofstream out(tmp, std::ios::trunc);
    out << value << '\n';
    out.close();
    if (out.fail()) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

inline bool remove_marker(const std::filesystem::path &path)
{
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return !ec;
}

} // namespace genesis::workspace::update
