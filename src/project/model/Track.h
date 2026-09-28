// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Ported from Concat's concat-project/src/model.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.
//
// Track model: the lane a clip sits on.

#pragma once

#include <string>

namespace genesis::project {

// A lane. Deliberately untyped: any media goes on any track.
// On disk: camelCase.
struct Track
{
    // Minted per timeline ("t5", or "T1".."T4" for the first timeline's
    // starter lanes); never shared between timelines.
    std::string id;
    // Video clips on this track are left out of the composite when false.
    bool visible = true;
    // Audio on this track is silent when true.
    bool muted = false;

    Track() = default;

    bool operator==(const Track &) const = default;
};

} // namespace genesis::project
