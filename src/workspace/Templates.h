// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Derived from Concat's concat-host/src/templates.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.

#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace genesis::project {
struct Project;
class Editor;
} // namespace genesis::project

namespace genesis::workspace {

// What a template file holds once saved, for the caller's feedback.
struct TemplateInfo
{
    // The template's display name, written as the document's `name`.
    std::string name;
    // Bin items sliced in with the selection's media.
    std::size_t media = 0;
    // Lanes the selection occupied, top to bottom.
    std::size_t tracks = 0;
    // Clips sliced in.
    std::size_t clips = 0;
};

// Saves a cut of `source` as a template file at `file`.
//
// A template is one project document - the engine's own `to_document`, not a
// second format - holding a single timeline: exactly the lanes the selection
// occupied (top to bottom) and the selected clips on them, stripped to the
// placement instantiation promises to restore. `clip_ids` names clips of the
// active timeline; a clip on another timeline, or an unknown id, is skipped.
//
// The round-trip contract is placement, nothing more: each clip's media,
// lane, start, duration, kind and name come back. What a template does NOT
// carry is everything else a clip has - the in-point, volume, fades,
// transform, effects, keys and transitions. A selection naming a title or a
// layer (no media behind it) is refused rather than silently thinned, so a
// saved template never promises more than it keeps.
std::expected<TemplateInfo, std::string> save_template(const project::Project &source,
                                                       const std::vector<std::string> &clip_ids,
                                                       std::string_view name,
                                                       const std::filesystem::path &file);

// Instantiates the template at `file` into `editor`, and returns the new
// timeline's id.
//
// Everything goes through `Editor::apply`, never straight into the project,
// so the whole import is undoable: the media, a fresh timeline, the lanes and
// the clips are one command each, and undoing them in reverse returns the
// project to what it was. A template whose media files no longer exist on
// disk still instantiates - the bin entries carry their probed metadata, and
// importing does not touch the filesystem - so a cut outlives its footage the
// way a project does (R9). A missing, unreadable or unparseable file is
// refused with a reason, leaving `editor` untouched.
std::expected<std::string, std::string> instantiate_template(project::Editor &editor,
                                                             const std::filesystem::path &file);

} // namespace genesis::workspace
