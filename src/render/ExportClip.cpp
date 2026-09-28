// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "render/ExportClip.h"

namespace genesis::render {

ExportClip ExportClip::blank(ClipKind kind, genesis::core::Rational start,
                             genesis::core::Rational duration, std::size_t track)
{
    ExportClip clip;
    clip.kind = kind;
    clip.start = start;
    clip.duration = duration;
    clip.track = track;
    return clip;
}

} // namespace genesis::render
