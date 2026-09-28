// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Ported from Concat's concat-project/src/commands/mod.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.

#include "project/IdMinter.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string>
#include <system_error>

#include "project/Project.h"
#include "project/model/Clip.h"
#include "project/model/Media.h"
#include "project/model/Timeline.h"
#include "project/model/Track.h"

namespace genesis::project {

std::string IdMinter::next(std::string_view prefix)
{
    const std::string id = peek(prefix);
    ++counter_;
    return id;
}

std::string IdMinter::peek(std::string_view prefix) const
{
    return std::string(prefix) + std::to_string(counter_ + 1);
}

void IdMinter::adopt(std::string_view id)
{
    // The trailing run of ASCII digits. Rust collects it by reversing the id,
    // taking digits, and reversing back; walking backwards from the end reads
    // the same suffix.
    std::size_t first = id.size();
    while (first > 0 && id[first - 1] >= '0' && id[first - 1] <= '9') {
        --first;
    }
    const std::string_view digits = id.substr(first);
    if (digits.empty()) {
        return;
    }
    // Parsed without throwing; a suffix wider than a u64 fails the same way
    // Rust's `parse::<u64>()` does on overflow, and is not adopted.
    std::uint64_t value = 0;
    const char *const begin = digits.data();
    const char *const end = begin + digits.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec == std::errc{ } && ptr == end) {
        counter_ = std::max(counter_, value);
    }
}

void IdMinter::adopt_project(const Project &project)
{
    for (const MediaItem &item : project.media) {
        adopt(item.id);
    }
    for (const Timeline &timeline : project.timelines) {
        adopt(timeline.id);
        for (const Track &track : timeline.tracks) {
            adopt(track.id);
        }
        for (const Clip &clip : timeline.clips) {
            adopt(clip.id);
        }
    }
}

} // namespace genesis::project
