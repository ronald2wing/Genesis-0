// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include "core/ClipTimeline.h"

// GES types are forward-declared so this header stays lean; only the adapter
// .cpp files include the full GES headers.
typedef struct _GESTimeline GESTimeline;
typedef struct _GstElement GstElement;

namespace genesis::core {
struct ShaderPass;
}

namespace genesis::render {
struct BuiltTimeline;
}

namespace genesis::adapters::engine::ges {

// Compiles the host graph into a playable GESTimeline: one GESLayer per host
// track, one GESClip per visual/audio clip via ges_layer_add_asset, and one
// video plus one audio GESTrack so preroll, seek, and position answer without
// further setup. Returns nullptr on failure. The GESTimeline* is owned by the
// caller (gst_object_unref) - this is the adapter boundary, and GESTimeline is
// never handed back to host code.
//
// `BuiltTimeline` is self-contained: its clips carry the media paths, so the
// built graph is the whole contract and no `Project` is consulted.
GESTimeline *build_timeline(const genesis::render::BuiltTimeline &timeline);

// The frame rate the built timeline runs at, from the host's own rate.
genesis::core::FrameRate frame_rate_of(const genesis::render::BuiltTimeline &timeline);

// Builds the standalone reveal bin for one pass carrying a `reveal_map`: the
// routing branch a title's per-word reveal takes instead of a glshader. The
// bin's ghost sink "sink" takes the picture (sink_0), an internal "map" appsrc
// feeds the reveal map up the GRAY8 -> RGBA GLMemory chain into sink_1, and the
// ghost src "src" emits the revealed picture. The texturemix element samples
// both in one fragment shader, which stock glshader cannot express
// (docs/decisions/effects-execution.md §6). The caller feeds the map by pushing
// width*height GRAY8 bytes plus EOS into "map" (gst_bin_get_by_name).
//
// Returns nullptr when the pass carries no reveal map, or when the texturemix
// element or the bin cannot be built. The caller owns the returned element
// (gst_object_unref).
GstElement *make_reveal_bin(const genesis::core::ShaderPass &pass);

} // namespace genesis::adapters::engine::ges
