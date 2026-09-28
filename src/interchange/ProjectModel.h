// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The project-model helpers every interchange adapter shares. An adapter reads
// and writes the typed model, so these three lookups are the whole vocabulary
// it needs; keeping one copy means the adapters cannot disagree about which
// timeline is "the" timeline or what "settled" means.

#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include "project/Document.h"
#include "project/Project.h"
#include "project/model/Media.h"
#include "project/model/Timeline.h"

namespace genesis::interchange {

// The timeline the commands act on: the active one, or the first when the id
// names no member. `Project::active()` degrades the same way, but it asserts a
// non-empty list while an import has to tolerate an empty one mid-build.
inline const project::Timeline *active_timeline(const project::Project &project)
{
    for (const project::Timeline &timeline : project.timelines) {
        if (timeline.id == project.active_timeline_id) {
            return &timeline;
        }
    }
    return project.timelines.empty() ? nullptr : &project.timelines.front();
}

// The bin item with this id, or null. An empty id is "no media", not a lookup.
inline const project::MediaItem *media_by_id(const project::Project &project, const std::string &id)
{
    if (id.empty()) {
        return nullptr;
    }
    for (const project::MediaItem &item : project.media) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

// Holds a synthesised project to the document layer's invariants: the import
// paths build in the typed model, then pass through the same writer and reader
// a document does, so there is one set of rules rather than a second one in
// every adapter.
inline std::optional<project::Project> settle_project(project::Project project)
{
    return project::from_document(project::to_document(project));
}

// The final path component of a slashed path, or the whole string when it has
// none. An adapter names an imported media item from its URL with this.
inline std::string basename_of(const std::string &path)
{
    const std::size_t separator = path.find_last_of('/');
    return separator == std::string::npos ? path : path.substr(separator + 1);
}

} // namespace genesis::interchange
