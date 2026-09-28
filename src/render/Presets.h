// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Named preset libraries: animation (keyframe) presets, camera-motion
// (Ken Burns) keyframe helpers, and colour / film-grain looks. The values
// are re-authored in C++ from two upstream semantic sources; no upstream
// code is copied:
//
//   * OpenShot openshot-qt src/classes/{animation_presets,camera_motion,
//     color_presets,film_grain_presets}.py - GNU GPL-3.0-or-later,
//     Copyright (c) 2008-2026 OpenShot Studios, LLC.
//   * Animate.css v4.1.1, the keyframe data the animation presets derive
//     from - MIT, Copyright (c) 2020 Daniel Eden.
//
// Re-authored 2026-09-30 as data and pure arithmetic under the combined
// GPL-3.0-or-later work (this file's SPDX license). Values that our
// property set (project::KeyProperty) or our effect packs cannot express
// are dropped, never invented; the drop set is documented in Presets.cpp.

#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "project/model/Clip.h"
#include "project/model/Effects.h"
#include "project/model/Keyframe.h"

namespace genesis::render {

// ---------------------------------------------------------------------------
// Animation presets
// ---------------------------------------------------------------------------

// One named animation preset: a run of clip keys (each a property, a point in
// the clip, a value in the property's own units, and an approach ease). The
// keys are ordered by property and then by `at`, matching Clip::sort_keys.
struct AnimationPreset
{
    std::string_view name;
    std::vector<project::ClipKey> keys;
};

// The named preset, or nullopt for a name this build does not know.
std::optional<AnimationPreset> animation_preset(std::string_view name);

// Every animation preset, in menu order.
const std::vector<AnimationPreset> &animation_presets();

// ---------------------------------------------------------------------------
// Camera motion (Ken Burns) keyframe helpers
// ---------------------------------------------------------------------------

// Which way the camera drifts. Auto picks the strongest crop axis from the
// source and project frames, the same fallback camera_motion.py makes.
enum class MotionDirection {
    Auto,
    LeftToRight,
    RightToLeft,
    TopToBottom,
    BottomToTop,
    PanLeft,
    PanRight,
    PanUp,
    PanDown,
};

// A pan/zoom pair as start and end values. Scale is the uniform factor over
// the fitted size (our model keys one scale, not per-axis); offsets are
// fractions of frame width and height, positive right and down. The caller
// keys the pair at the clip's head and tail (0.0 and 1.0), so it spans the
// clip's own duration.
struct CameraMotion
{
    double scale_start = 1.0;
    double scale_end = 1.0;
    double offset_x_start = 0.0;
    double offset_x_end = 0.0;
    double offset_y_start = 0.0;
    double offset_y_end = 0.0;
};

// A smart Ken Burns drift: zoom in or out while panning, sized so the frame
// stays covered and the pan has room. Mirrors camera_motion.py's
// ken_burns_keyframes arithmetic.
CameraMotion ken_burns(bool zoom_in, MotionDirection direction, double project_width,
                       double project_height, double source_width, double source_height,
                       double zoom = 1.22, double target_pan = 0.10, double max_pan = 0.24);

// A pan across the source with no zoom beyond what the pan needs to stay
// covered. Mirrors camera_pan_keyframes.
CameraMotion camera_pan(MotionDirection direction, double project_width, double project_height,
                        double source_width, double source_height, double target_pan = 0.18,
                        double edge_margin = 0.995);

// A centred push-in or pull-out with no drift. Mirrors push_pull_keyframes.
CameraMotion push_pull(bool zoom_in, double zoom = 1.2);

// ---------------------------------------------------------------------------
// Colour looks
// ---------------------------------------------------------------------------

// One named colour look, as the existing effect packs it maps onto with
// their parameter values. An empty `effects` is "reset" - no colour grade.
// A look is built only from packs that exist; a parameter the packs cannot
// express is dropped and documented in Presets.cpp, never faked.
struct ColourLook
{
    std::string_view name;
    std::vector<project::AppliedFilter> effects;
};

// The named look, or nullopt for a name this build does not know.
std::optional<ColourLook> colour_look(std::string_view name);

// Every colour look, in menu order.
const std::vector<ColourLook> &colour_looks();

// ---------------------------------------------------------------------------
// Film-grain looks
// ---------------------------------------------------------------------------

// One named film-grain look, as the genesis.film-grain pack with its
// `amount` parameter set. That pack exposes only the overall grain amount,
// so the per-channel and structural knobs the upstream presets carry are
// dropped; see Presets.cpp.
struct FilmGrainLook
{
    std::string_view name;
    project::AppliedFilter effect;
};

// The named look, or nullopt for a name this build does not know.
std::optional<FilmGrainLook> film_grain_look(std::string_view name);

// Every film-grain look, in menu order.
const std::vector<FilmGrainLook> &film_grain_looks();

} // namespace genesis::render
