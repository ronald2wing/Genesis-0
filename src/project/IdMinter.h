// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace genesis::project {

struct Project;

// Mints ids. Owned by the editor so restored projects advance it past every
// id a file already uses - the collision class `adopt_project` fixed in the
// UI is prevented here instead.
class IdMinter
{
public:
    // The next fresh id: the prefix ("c", "t", "tl", "m") plus a counter
    // shared across all prefixes, so no two ids ever share a number.
    std::string next(std::string_view prefix);

    // The id `next(prefix)` will produce, without consuming it. A host reads
    // this before building a Batch so it can name the id an AddMedia inside
    // the batch will mint - a paired AddClip then references it in one undo
    // step. Because the counter is shared and adopt_project() advances it past
    // every id a project already uses, the peeking id can never collide.
    std::string peek(std::string_view prefix) const;

    // Advances the counter past `id`'s numeric suffix, if it has one.
    void adopt(std::string_view id);

    // Adopts every id a restored project uses - media, timelines, tracks,
    // clips - so nothing minted afterwards can collide with the file.
    void adopt_project(const Project &project);

    // The counter, so the undo layer can snapshot and restore it around an
    // edit: it is not part of the project model, yet re-running a command
    // after undo must reproduce the same ids.
    std::uint64_t counter() const { return counter_; }
    void set_counter(std::uint64_t value) { counter_ = value; }

private:
    std::uint64_t counter_ = 0;
};

} // namespace genesis::project
