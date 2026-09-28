// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Ported from Concat's concat-project/src/model.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.
//
// Font model: a font the user added from disk.

#pragma once

#include <string>

namespace genesis::project {

// A font the user added from disk.
// On disk: camelCase.
struct CustomFont
{
    // The family name titles refer to; removal leaves referring clips
    // untouched so the face can come back when the file does.
    std::string family;
    // Where the font file lives on disk. Doubles as the duplicate check
    // when adding.
    std::string path;

    bool operator==(const CustomFont &) const = default;
};

} // namespace genesis::project
