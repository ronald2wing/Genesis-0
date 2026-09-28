// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace genesis::project {
struct VideoSettings;
}

namespace genesis::workspace {

// Everything needed to reopen a project: where it lives, what to call it,
// and the frame it renders at.
struct ProjectInfo
{
    // The project folder.
    std::string path;
    // The name shown in the title bar and the recents list.
    std::string name;
    // Output width in pixels.
    std::uint32_t width = 1920;
    // Output height in pixels.
    std::uint32_t height = 1080;
    // Frame rate as an exact fraction; the decimal is never stored.
    std::int64_t rate_num = 30;
    // Denominator of the frame rate.
    std::int64_t rate_den = 1;
    // Milliseconds since the epoch.
    std::uint64_t opened_at = 0;
    // Whether the user pinned this entry; a pinned entry is not evicted and
    // keeps its place.
    bool pinned = false;

    bool operator==(const ProjectInfo &) const = default;
};

// Long enough to be useful, short enough that the list stays scannable.
inline constexpr std::size_t MAX_RECENTS = 12;
// The recents file, inside the caller's config directory.
inline constexpr std::string_view RECENTS_FILE = "recents.json";

// Moves a project to the front of the recents list, replacing any entry with
// the same path, and trims the list to MAX_RECENTS. Best-effort: an
// unwritable config directory is not worth failing the launch over.
void add_recents(const std::filesystem::path &config, ProjectInfo project);

// The recents list, most recent first, with folders that no longer hold a
// project dropped. Filtering on read rather than pruning on write means a
// project on a drive that happens to be unplugged comes back when the drive
// does. A missing or corrupt list reads empty.
std::vector<ProjectInfo> list_recents(const std::filesystem::path &config);

// Removes one project from the list; the folder itself is left alone.
void remove_recents(const std::filesystem::path &config, std::string_view path);

// Sets the pinned flag on the entry with this path, in place: pinning never
// reorders or protects an entry from eviction, it only marks it.
void pin_recents(const std::filesystem::path &config, std::string_view path, bool pinned);

// The manifest file this project uses.
std::filesystem::path manifest_path(const std::filesystem::path &root);

// Whether `root` holds a project: it has a manifest.
bool is_project(const std::filesystem::path &root);

// Turns a project name into something a filesystem will accept: control and
// reserved characters become dashes, and a name that would end up empty is
// "Untitled project".
std::string folder_name(std::string_view name);

// Creates the project folder and writes its settings-only manifest. Refuses
// a folder that already holds a manifest, so reusing a name cannot clobber an
// edit.
std::expected<ProjectInfo, std::string> create_project(const std::filesystem::path &location,
                                                       std::string_view name,
                                                       const project::VideoSettings &video);

// Reads an existing project's settings back. Errors on a manifest that cannot
// be read or parsed, and on a zero frame-rate denominator.
std::expected<ProjectInfo, std::string> open_project(const std::filesystem::path &root);

} // namespace genesis::workspace
