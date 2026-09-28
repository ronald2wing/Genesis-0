// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Adds an extracted top effect to a clip. Mask and reveal effects both build a
// floating GESBaseEffect*, then hand it over here: sink the floating reference,
// add at the top, drop the local reference, so both of
// ges_clip_add_top_effect's failure paths keep the effect alive until the clip
// takes its own reference. Must run before add_track.

#pragma once

#include <ges/ges.h>

namespace genesis::adapters::engine::ges {

inline bool install_top_effect(GESClip *clip, GESBaseEffect *effect)
{
    if (clip == nullptr || effect == nullptr) {
        return false;
    }
    g_object_ref_sink(effect);
    GError *error = nullptr;
    const gboolean added = ges_clip_add_top_effect(clip, effect, -1, &error);
    gst_object_unref(effect);
    if (!added) {
        g_clear_error(&error);
        return false;
    }
    return true;
}

} // namespace genesis::adapters::engine::ges
