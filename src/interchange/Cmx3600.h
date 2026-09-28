// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// CMX-3600 EDL over the project model. Lossy by construction: an EDL carries
// no effects and no transitions, and only one video lane plus one audio lane,
// so this is a format for handing a cut to another editor rather than a
// lossless round trip.

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace genesis::project {
struct Project;
struct VideoSettings;
} // namespace genesis::project

namespace genesis::interchange {

// The active timeline as CMX-3600 text. Empty when the project has no
// timeline.
std::string to_cmx3600(const project::Project &project);

// Reads a CMX-3600 EDL into a project. An EDL carries no frame rate, so the
// caller supplies the rate the timecodes were written at (normally the open
// project's). A reel is not a file: each distinct reel becomes a media item
// whose path is the reel name, so the cut lands with its timing intact and the
// user relinks the media afterwards. On failure returns nullopt and fills
// `error` with a message naming the problem.
std::optional<project::Project>
from_cmx3600(std::string_view text, const project::VideoSettings &video, std::string &error);

} // namespace genesis::interchange