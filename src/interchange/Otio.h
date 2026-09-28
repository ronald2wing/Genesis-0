// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// OpenTimelineIO over the project model. The adapter reads and writes the
// typed model rather than hand-copying JSON keys, so it cannot drift from the
// schema the document layer owns; a file this build wrote round-trips exactly
// because the source document travels with it.

#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace genesis::project {
struct Project;
struct VideoSettings;
} // namespace genesis::project

namespace genesis::interchange {

// The active timeline as an OTIO Timeline document. Empty JSON when the
// project has no timeline.
nlohmann::json to_otio(const project::Project &project);

// Reads an OTIO Timeline back into a project. Relative media URLs resolve
// against `source_path`'s directory. OTIO carries the rate per rational, not
// per timeline, so a stills-only or all-gap timeline may declare none; the
// caller supplies the rate (and frame) to fall back on, normally the open
// project's. On failure returns nullopt and fills `error` with a message
// naming the problem.
std::optional<project::Project> from_otio(const nlohmann::json &timeline,
                                          const std::filesystem::path &source_path,
                                          const project::VideoSettings &fallback,
                                          std::string &error);

} // namespace genesis::interchange
