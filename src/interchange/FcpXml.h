// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// FCPXML export over the project model. Export-only and deliberately scoped:
// the primary video spine is written (with gaps and asset references), and
// audio travels as connected clips on lane 1 above it; effects, transitions
// and titles are not, because a half-mapped FCPXML is worse than an honest
// flat cut. OTIO is the format for a lossless round trip.

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace genesis::project {
struct Project;
struct VideoSettings;
} // namespace genesis::project

namespace genesis::interchange {

// The active timeline as an FCPXML 1.9 document. Empty when the project has no
// timeline.
std::string to_fcpxml(const project::Project &project);

// Reads an FCPXML document back into a project. FCPXML carries no reliable
// project-level rate for a sequence that declares none, so the caller supplies
// the target rate via `video`. On failure returns nullopt and fills `error`
// with a message naming the problem.
std::optional<project::Project> from_fcpxml(std::string_view xml,
                                            const std::filesystem::path &source_path,
                                            const project::VideoSettings &video,
                                            std::string &error);

} // namespace genesis::interchange
