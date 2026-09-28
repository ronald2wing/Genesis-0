// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// See Presets.h for provenance: the values here are re-authored from
// OpenShot openshot-qt (GPL-3.0-or-later) and Animate.css v4.1.1 (MIT),
// 2026-09-30, under this work's GPL-3.0-or-later license.

#include "render/Presets.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <string>
#include <utility>

namespace genesis::render {

namespace {

using genesis::project::AppliedFilter;
using genesis::project::ClipKey;
using genesis::project::KeyEase;
using genesis::project::KeyProperty;

// ---------------------------------------------------------------------------
// Animation presets
// ---------------------------------------------------------------------------

// OpenShot stores keyframe times as frames 1..31 at 30 fps; frame 1 is the
// clip's head, frame 31 its tail. This converts a frame to a clip fraction.
constexpr double at_frame(int frame)
{
    return static_cast<double>(frame - 1) / 30.0;
}

// The one easing the kept animation presets carry (the bounce entrances and
// the attention "bounce" use it; everything else is linear). The upstream
// names four others, but none of the presets our property set can express
// use them, so they are not re-authored here.
constexpr KeyEase EASE_OUT_CUBIC = KeyEase{ std::array<double, 4>{ 0.215, 0.61, 0.355, 1.0 } };

ClipKey key(KeyProperty property, int frame, double value, KeyEase ease)
{
    return ClipKey{ property, at_frame(frame), value, ease };
}

// OpenShot's alpha/scale_x/scale_y/location_x/location_y/rotation map onto
// our KeyProperty as Opacity/Scale/Scale/OffsetX/OffsetY/Rotation. Presets
// whose effect needs a channel our property set lacks are dropped:
//
//   * bounce, rubberBand - non-uniform scale (scale_x != scale_y, or a
//     single-axis scale). Our model keys one uniform Scale.
//   * bounceIn{Down,Left,Right,Up}, bounceOut{Down,Left,Right,Up} - the
//     directional bounces drive a single scale axis; same reason.
//   * jello - drives shear_x/shear_y, which our property set has no
//     equivalent for.
//
// Everything else maps: uniform scale, offsets, rotation, opacity.

// A run of keys for one property. Shortens the table below.
struct Prop
{
    KeyProperty property;
    std::initializer_list<std::pair<int, double>> keys;
    KeyEase ease = KeyEase::linear();
};

std::vector<ClipKey> keys_of(std::initializer_list<Prop> props)
{
    std::vector<ClipKey> out;
    for (const Prop &prop : props) {
        for (const auto &[frame, value] : prop.keys) {
            out.push_back(key(prop.property, frame, value, prop.ease));
        }
    }
    return out;
}

AnimationPreset preset(std::string_view name, std::vector<ClipKey> keys)
{
    return AnimationPreset{ name, std::move(keys) };
}

} // namespace

const std::vector<AnimationPreset> &animation_presets()
{
    using P = KeyProperty;
    static const std::vector<AnimationPreset> presets = {
        // Attention seekers.
        preset("flash",
               keys_of({ { P::Opacity,
                           { { 1, 1 }, { 8, 0 }, { 16, 1 }, { 24, 0 }, { 31, 1 } } } })),
        preset("pulse", keys_of({ { P::Scale, { { 1, 1 }, { 16, 1.05 }, { 31, 1 } } } })),
        preset("heartBeat",
               keys_of({ { P::Scale,
                           { { 1, 1 }, { 5, 1.3 }, { 9, 1 }, { 14, 1.3 }, { 22, 1 } } } })),
        preset("swing",
               keys_of({ { P::Rotation,
                           { { 7, 15 }, { 13, -10 }, { 19, 5 }, { 25, -5 }, { 31, 0 } } } })),
        preset("tada",
               keys_of({ { P::Scale,
                           { { 1, 1 },
                             { 4, 0.9 },
                             { 7, 0.9 },
                             { 10, 1.1 },
                             { 13, 1.1 },
                             { 16, 1.1 },
                             { 19, 1.1 },
                             { 22, 1.1 },
                             { 25, 1.1 },
                             { 28, 1.1 },
                             { 31, 1 } } },
                         { P::Rotation,
                           { { 4, -3 },
                             { 7, -3 },
                             { 10, 3 },
                             { 13, -3 },
                             { 16, 3 },
                             { 19, -3 },
                             { 22, 3 },
                             { 25, -3 },
                             { 28, 3 } } } })),
        preset("wobble",
               keys_of({ { P::Rotation,
                           { { 6, -5 }, { 10, 3 }, { 14, -3 }, { 19, 2 }, { 24, -1 } } },
                         { P::OffsetX,
                           { { 1, 0 },
                             { 6, -0.25 },
                             { 10, 0.2 },
                             { 14, -0.15 },
                             { 19, 0.1 },
                             { 24, -0.05 },
                             { 31, 0 } } } })),
        preset("shakeX",
               keys_of({ { P::OffsetX,
                           { { 1, 0 },
                             { 4, -0.005208 },
                             { 7, 0.005208 },
                             { 10, -0.005208 },
                             { 13, 0.005208 },
                             { 16, -0.005208 },
                             { 19, 0.005208 },
                             { 22, -0.005208 },
                             { 25, 0.005208 },
                             { 28, -0.005208 },
                             { 31, 0 } } } })),
        preset("shakeY",
               keys_of({ { P::OffsetY,
                           { { 1, 0 },
                             { 4, -0.009259 },
                             { 7, 0.009259 },
                             { 10, -0.009259 },
                             { 13, 0.009259 },
                             { 16, -0.009259 },
                             { 19, 0.009259 },
                             { 22, -0.009259 },
                             { 25, 0.009259 },
                             { 28, -0.009259 },
                             { 31, 0 } } } })),

        // Back entrances.
        preset("backInDown",
               keys_of({ { P::Opacity, { { 1, 0.7 }, { 25, 0.7 }, { 31, 1 } } },
                         { P::Scale, { { 1, 0.7 }, { 25, 0.7 }, { 31, 1 } } },
                         { P::OffsetY, { { 1, -1.5 }, { 25, 0 } } } })),
        preset("backInLeft",
               keys_of({ { P::Opacity, { { 1, 0.7 }, { 25, 0.7 }, { 31, 1 } } },
                         { P::Scale, { { 1, 0.7 }, { 25, 0.7 }, { 31, 1 } } },
                         { P::OffsetX, { { 1, -1.5 }, { 25, 0 } } } })),
        preset("backInRight",
               keys_of({ { P::Opacity, { { 1, 0.7 }, { 25, 0.7 }, { 31, 1 } } },
                         { P::Scale, { { 1, 0.7 }, { 25, 0.7 }, { 31, 1 } } },
                         { P::OffsetX, { { 1, 1.5 }, { 25, 0 } } } })),
        preset("backInUp",
               keys_of({ { P::Opacity, { { 1, 0.7 }, { 25, 0.7 }, { 31, 1 } } },
                         { P::Scale, { { 1, 0.7 }, { 25, 0.7 }, { 31, 1 } } },
                         { P::OffsetY, { { 1, 1.5 }, { 25, 0 } } } })),

        // Back exits.
        preset("backOutDown",
               keys_of({ { P::Opacity, { { 1, 1 }, { 7, 0.7 }, { 31, 0.7 } } },
                         { P::Scale, { { 1, 1 }, { 7, 0.7 }, { 31, 0.7 } } },
                         { P::OffsetY, { { 7, 0 }, { 31, 1.5 } } } })),
        preset("backOutLeft",
               keys_of({ { P::Opacity, { { 1, 1 }, { 7, 0.7 }, { 31, 0.7 } } },
                         { P::Scale, { { 1, 1 }, { 7, 0.7 }, { 31, 0.7 } } },
                         { P::OffsetX, { { 7, 0 }, { 31, -1.5 } } } })),
        preset("backOutRight",
               keys_of({ { P::Opacity, { { 1, 1 }, { 7, 0.7 }, { 31, 0.7 } } },
                         { P::Scale, { { 1, 1 }, { 7, 0.7 }, { 31, 0.7 } } },
                         { P::OffsetX, { { 7, 0 }, { 31, 1.5 } } } })),
        preset("backOutUp",
               keys_of({ { P::Opacity, { { 1, 1 }, { 7, 0.7 }, { 31, 0.7 } } },
                         { P::Scale, { { 1, 1 }, { 7, 0.7 }, { 31, 0.7 } } },
                         { P::OffsetY, { { 7, 0 }, { 31, -1.5 } } } })),

        // Bouncing entrances: the directional ones drive a single scale axis
        // and are dropped; the base bounce is uniform and kept.
        preset("bounceIn",
               keys_of({ { P::Opacity, { { 1, 0 }, { 19, 1 }, { 31, 1 } }, EASE_OUT_CUBIC },
                         { P::Scale,
                           { { 1, 0.3 },
                             { 7, 1.1 },
                             { 13, 0.9 },
                             { 19, 1.03 },
                             { 25, 0.97 },
                             { 31, 1 } },
                           EASE_OUT_CUBIC } })),

        // Bouncing exits: same - the base is uniform, the directional ones
        // drive a single scale axis and are dropped.
        preset("bounceOut",
               keys_of({ { P::Opacity, { { 16, 1 }, { 18, 1 }, { 31, 0 } } },
                         { P::Scale, { { 7, 0.9 }, { 16, 1.1 }, { 18, 1.1 }, { 31, 0.3 } } } })),

        // Fading entrances.
        preset("fadeIn", keys_of({ { P::Opacity, { { 1, 0 }, { 31, 1 } } } })),
        preset("fadeInDown",
               keys_of({ { P::Opacity, { { 1, 0 }, { 31, 1 } } },
                         { P::OffsetY, { { 1, -1 }, { 31, 0 } } } })),
        preset("fadeInDownBig",
               keys_of({ { P::Opacity, { { 1, 0 }, { 31, 1 } } },
                         { P::OffsetY, { { 1, -3 }, { 31, 0 } } } })),
        preset("fadeInLeft",
               keys_of({ { P::Opacity, { { 1, 0 }, { 31, 1 } } },
                         { P::OffsetX, { { 1, -1 }, { 31, 0 } } } })),
        preset("fadeInLeftBig",
               keys_of({ { P::Opacity, { { 1, 0 }, { 31, 1 } } },
                         { P::OffsetX, { { 1, -3 }, { 31, 0 } } } })),
        preset("fadeInRight",
               keys_of({ { P::Opacity, { { 1, 0 }, { 31, 1 } } },
                         { P::OffsetX, { { 1, 1 }, { 31, 0 } } } })),
        preset("fadeInRightBig",
               keys_of({ { P::Opacity, { { 1, 0 }, { 31, 1 } } },
                         { P::OffsetX, { { 1, 3 }, { 31, 0 } } } })),
        preset("fadeInUp",
               keys_of({ { P::Opacity, { { 1, 0 }, { 31, 1 } } },
                         { P::OffsetY, { { 1, 1 }, { 31, 0 } } } })),
        preset("fadeInUpBig",
               keys_of({ { P::Opacity, { { 1, 0 }, { 31, 1 } } },
                         { P::OffsetY, { { 1, 3 }, { 31, 0 } } } })),
        preset("fadeInTopLeft",
               keys_of({ { P::Opacity, { { 1, 0 }, { 31, 1 } } },
                         { P::OffsetX, { { 1, -1 }, { 31, 0 } } },
                         { P::OffsetY, { { 1, -1 }, { 31, 0 } } } })),
        preset("fadeInTopRight",
               keys_of({ { P::Opacity, { { 1, 0 }, { 31, 1 } } },
                         { P::OffsetX, { { 1, 1 }, { 31, 0 } } },
                         { P::OffsetY, { { 1, -1 }, { 31, 0 } } } })),
        preset("fadeInBottomLeft",
               keys_of({ { P::Opacity, { { 1, 0 }, { 31, 1 } } },
                         { P::OffsetX, { { 1, -1 }, { 31, 0 } } },
                         { P::OffsetY, { { 1, 1 }, { 31, 0 } } } })),
        preset("fadeInBottomRight",
               keys_of({ { P::Opacity, { { 1, 0 }, { 31, 1 } } },
                         { P::OffsetX, { { 1, 1 }, { 31, 0 } } },
                         { P::OffsetY, { { 1, 1 }, { 31, 0 } } } })),

        // Fading exits.
        preset("fadeOut", keys_of({ { P::Opacity, { { 1, 1 }, { 31, 0 } } } })),
        preset("fadeOutDown",
               keys_of({ { P::Opacity, { { 1, 1 }, { 31, 0 } } }, { P::OffsetY, { { 31, 1 } } } })),
        preset("fadeOutDownBig",
               keys_of({ { P::Opacity, { { 1, 1 }, { 31, 0 } } }, { P::OffsetY, { { 31, 3 } } } })),
        preset("fadeOutLeft",
               keys_of({ { P::Opacity, { { 1, 1 }, { 31, 0 } } },
                         { P::OffsetX, { { 31, -1 } } } })),
        preset("fadeOutLeftBig",
               keys_of({ { P::Opacity, { { 1, 1 }, { 31, 0 } } },
                         { P::OffsetX, { { 31, -3 } } } })),
        preset("fadeOutRight",
               keys_of({ { P::Opacity, { { 1, 1 }, { 31, 0 } } }, { P::OffsetX, { { 31, 1 } } } })),
        preset("fadeOutRightBig",
               keys_of({ { P::Opacity, { { 1, 1 }, { 31, 0 } } }, { P::OffsetX, { { 31, 3 } } } })),
        preset("fadeOutUp",
               keys_of({ { P::Opacity, { { 1, 1 }, { 31, 0 } } },
                         { P::OffsetY, { { 31, -1 } } } })),
        preset("fadeOutUpBig",
               keys_of({ { P::Opacity, { { 1, 1 }, { 31, 0 } } },
                         { P::OffsetY, { { 31, -3 } } } })),
        preset("fadeOutTopLeft",
               keys_of({ { P::Opacity, { { 1, 1 }, { 31, 0 } } },
                         { P::OffsetX, { { 1, 0 }, { 31, -1 } } },
                         { P::OffsetY, { { 1, 0 }, { 31, -1 } } } })),
        preset("fadeOutTopRight",
               keys_of({ { P::Opacity, { { 1, 1 }, { 31, 0 } } },
                         { P::OffsetX, { { 1, 0 }, { 31, 1 } } },
                         { P::OffsetY, { { 1, 0 }, { 31, -1 } } } })),
        preset("fadeOutBottomRight",
               keys_of({ { P::Opacity, { { 1, 1 }, { 31, 0 } } },
                         { P::OffsetX, { { 1, 0 }, { 31, 1 } } },
                         { P::OffsetY, { { 1, 0 }, { 31, 1 } } } })),
        preset("fadeOutBottomLeft",
               keys_of({ { P::Opacity, { { 1, 1 }, { 31, 0 } } },
                         { P::OffsetX, { { 1, 0 }, { 31, -1 } } },
                         { P::OffsetY, { { 1, 0 }, { 31, 1 } } } })),
    };
    return presets;
}

std::optional<AnimationPreset> animation_preset(std::string_view name)
{
    for (const AnimationPreset &preset : animation_presets()) {
        if (preset.name == name) {
            return preset;
        }
    }
    return std::nullopt;
}

namespace {

// ---------------------------------------------------------------------------
// Camera motion (Ken Burns) arithmetic, mirroring camera_motion.py
// ---------------------------------------------------------------------------

double positive_float(double value, double fallback)
{
    return (value > 0.0 && std::isfinite(value)) ? value : fallback;
}

struct CropBase
{
    double width;
    double height;
};

// Source size after SCALE_CROP with scale at 1.0: the larger of the two
// frame aspects, fitted.
CropBase crop_base_size(double project_width, double project_height, double source_width,
                        double source_height)
{
    const double pw = positive_float(project_width, 1920.0);
    const double ph = positive_float(project_height, 1080.0);
    const double sw = positive_float(source_width, pw);
    const double sh = positive_float(source_height, ph);
    const double project_aspect = pw / ph;
    const double source_aspect = sw / sh;
    if (source_aspect >= project_aspect) {
        return CropBase{ ph * source_aspect, ph };
    }
    return CropBase{ pw, pw / source_aspect };
}

// Largest normalized location that keeps SCALE_CROP edges covered.
double safe_location(double canvas_size, double base_size, double scale)
{
    const double scaled = std::max(0.0001, base_size) * std::max(0.001, scale);
    canvas_size = std::max(0.0001, canvas_size);
    if (scaled <= canvas_size) {
        return 0.0;
    }
    return (scaled - canvas_size) / (scaled + canvas_size);
}

// Minimum scale needed for `target` normalized pan room on one axis.
double scale_for_safe_location(double canvas_size, double base_size, double target)
{
    target = std::clamp(target, 0.0, 0.9);
    canvas_size = std::max(0.0001, canvas_size);
    base_size = std::max(0.0001, base_size);
    return (canvas_size * (1.0 + target)) / (base_size * (1.0 - target));
}

bool is_horizontal(MotionDirection direction)
{
    switch (direction) {
    case MotionDirection::PanLeft:
    case MotionDirection::PanRight:
    case MotionDirection::LeftToRight:
    case MotionDirection::RightToLeft:
        return true;
    case MotionDirection::Auto:
    case MotionDirection::TopToBottom:
    case MotionDirection::BottomToTop:
    case MotionDirection::PanUp:
    case MotionDirection::PanDown:
        return false;
    }
    return false;
}

// The strongest natural crop axis, for the Auto direction.
MotionDirection auto_direction(double project_width, double project_height, double source_width,
                               double source_height)
{
    const double pw = positive_float(project_width, 1920.0);
    const double ph = positive_float(project_height, 1080.0);
    const CropBase base = crop_base_size(pw, ph, source_width, source_height);
    const double x_room = safe_location(pw, base.width, 1.0);
    const double y_room = safe_location(ph, base.height, 1.0);
    if (x_room > y_room + 0.03) {
        return MotionDirection::LeftToRight;
    }
    if (y_room > x_room + 0.03) {
        return MotionDirection::BottomToTop;
    }
    return MotionDirection::LeftToRight;
}

// The (start, end) pan magnitude for a direction, as fractions of the canvas.
std::pair<double, double> pan_endpoints(MotionDirection direction, double magnitude)
{
    magnitude = std::max(0.0, magnitude);
    switch (direction) {
    case MotionDirection::PanLeft:
    case MotionDirection::RightToLeft:
    case MotionDirection::PanUp:
    case MotionDirection::BottomToTop:
        return { -magnitude, magnitude };
    case MotionDirection::PanRight:
    case MotionDirection::LeftToRight:
    case MotionDirection::PanDown:
    case MotionDirection::TopToBottom:
        return { magnitude, -magnitude };
    case MotionDirection::Auto:
        return { 0.0, 0.0 };
    }
    return { 0.0, 0.0 };
}

// The upstream's `target_pan if safe else 0.0`: a magnitude that fell to
// zero still gets `target_pan`, so a pan on an axis with no crop room at the
// resting scale is not a no-op.
double magnitude_from(double safe, double target_pan)
{
    return std::max(safe, safe == 0.0 ? 0.0 : target_pan);
}

} // namespace

CameraMotion ken_burns(bool zoom_in, MotionDirection direction, double project_width,
                       double project_height, double source_width, double source_height,
                       double zoom, double target_pan, double max_pan)
{
    const double pw = positive_float(project_width, 1920.0);
    const double ph = positive_float(project_height, 1080.0);
    const CropBase base = crop_base_size(pw, ph, source_width, source_height);
    if (direction == MotionDirection::Auto) {
        direction = auto_direction(pw, ph, source_width, source_height);
    }
    const bool horizontal = is_horizontal(direction);
    const double canvas = horizontal ? pw : ph;
    const double base_axis = horizontal ? base.width : base.height;

    zoom = std::max(zoom, scale_for_safe_location(canvas, base_axis, target_pan));

    const double start_scale = zoom_in ? 1.0 : zoom;
    const double end_scale = zoom_in ? zoom : 1.0;
    const double start_safe =
            std::min(safe_location(canvas, base_axis, start_scale) * 0.82, max_pan);
    const double end_safe = std::min(safe_location(canvas, base_axis, end_scale) * 0.82, max_pan);
    const double start_mag =
            pan_endpoints(direction, std::min(magnitude_from(start_safe, target_pan), max_pan))
                    .first;
    const double end_mag =
            pan_endpoints(direction, std::min(magnitude_from(end_safe, target_pan), max_pan))
                    .second;

    CameraMotion motion;
    motion.scale_start = start_scale;
    motion.scale_end = end_scale;
    if (horizontal) {
        motion.offset_x_start = start_mag;
        motion.offset_x_end = end_mag;
    } else {
        motion.offset_y_start = start_mag;
        motion.offset_y_end = end_mag;
    }
    return motion;
}

CameraMotion camera_pan(MotionDirection direction, double project_width, double project_height,
                        double source_width, double source_height, double target_pan,
                        double edge_margin)
{
    const double pw = positive_float(project_width, 1920.0);
    const double ph = positive_float(project_height, 1080.0);
    if (direction == MotionDirection::Auto) {
        direction = auto_direction(pw, ph, source_width, source_height);
    }
    const CropBase base = crop_base_size(pw, ph, source_width, source_height);
    const bool horizontal = is_horizontal(direction);
    const double canvas = horizontal ? pw : ph;
    const double base_axis = horizontal ? base.width : base.height;

    const double natural_room = safe_location(canvas, base_axis, 1.0);
    double scale = 1.0;
    double magnitude = 0.0;
    if (natural_room >= target_pan) {
        magnitude = natural_room * edge_margin;
    } else {
        scale = std::max(1.0, scale_for_safe_location(canvas, base_axis, target_pan));
        magnitude = safe_location(canvas, base_axis, scale) * edge_margin;
    }

    const std::pair<double, double> start_end = pan_endpoints(direction, magnitude);
    CameraMotion motion;
    motion.scale_start = scale;
    motion.scale_end = scale;
    if (horizontal) {
        motion.offset_x_start = start_end.first;
        motion.offset_x_end = start_end.second;
    } else {
        motion.offset_y_start = start_end.first;
        motion.offset_y_end = start_end.second;
    }
    return motion;
}

CameraMotion push_pull(bool zoom_in, double zoom)
{
    CameraMotion motion;
    motion.scale_start = zoom_in ? 1.0 : zoom;
    motion.scale_end = zoom_in ? zoom : 1.0;
    return motion;
}

namespace {

// ---------------------------------------------------------------------------
// Colour and film-grain looks
// ---------------------------------------------------------------------------

AppliedFilter effect(std::string id, std::initializer_list<std::pair<std::string, double>> params)
{
    AppliedFilter filter = AppliedFilter::create(std::move(id));
    for (const auto &[name, value] : params) {
        filter.params[name] = value;
    }
    return filter;
}

} // namespace

const std::vector<ColourLook> &colour_looks()
{
    // Each look maps OpenShot's ColorGrade scalars onto the packs that exist.
    // Units that correspond exactly map; the rest are dropped, not invented:
    //
    //   * exposure  (stops)  -> genesis.exposure `stops` (same unit)
    //   * contrast  (0..1)   -> genesis.contrast `amount` (percent; *100)
    //   * saturation (mult)  -> genesis.saturation `amount` (percent; (s-1)*100)
    //   * temperature (warm) -> genesis.temperature `temperature` (Kelvin;
    //                           re-authored +0.18 warm as 5600 K over the
    //                           6500 K neutral; a documented approximation)
    //   * tint      (0..1)   -> genesis.tint `amount` (percent; *100)
    //
    // Dropped because no pack expresses them: highlights, shadows, and the
    // tone curves (no pack). Vibrance is dropped too - genesis.vibrance's
    // `amount` is a 0.1..2 strength with no neutral-at-0, not an additive
    // amount, so OpenShot's additive vibrance cannot be expressed faithfully.
    static const std::vector<ColourLook> looks = {
        { "reset", { } },
        { "auto_contrast", { effect("genesis.contrast", { { "amount", 18.0 } }) } },
        { "lift_shadows",
          { effect("genesis.exposure", { { "stops", 0.08 } }),
            effect("genesis.contrast", { { "amount", -3.0 } }) } },
        { "warm_up",
          { effect("genesis.temperature", { { "temperature", 5600.0 } }),
            effect("genesis.tint", { { "amount", 3.0 } }),
            effect("genesis.saturation", { { "amount", 5.0 } }) } },
        { "boost_color",
          { effect("genesis.contrast", { { "amount", 8.0 } }),
            effect("genesis.saturation", { { "amount", 18.0 } }) } },
    };
    return looks;
}

std::optional<ColourLook> colour_look(std::string_view name)
{
    for (const ColourLook &look : colour_looks()) {
        if (look.name == name) {
            return look;
        }
    }
    return std::nullopt;
}

const std::vector<FilmGrainLook> &film_grain_looks()
{
    // OpenShot's FilmGrain presets carry a dozen structural knobs (size,
    // softness, clump, per-band shadows/midtones/highlights, colour amount,
    // evolution, coherence). genesis.film-grain exposes only the overall
    // `amount` (0..100 %), so each look maps its upstream amount as a
    // percentage and the rest is dropped.
    static const std::vector<FilmGrainLook> looks = {
        { "35mm_fine", effect("genesis.film-grain", { { "amount", 14.0 } }) },
        { "35mm_classic", effect("genesis.film-grain", { { "amount", 24.0 } }) },
        { "35mm_gritty", effect("genesis.film-grain", { { "amount", 34.0 } }) },
        { "16mm_classic", effect("genesis.film-grain", { { "amount", 42.0 } }) },
        { "super_8", effect("genesis.film-grain", { { "amount", 62.0 } }) },
        { "high_iso", effect("genesis.film-grain", { { "amount", 52.0 } }) },
    };
    return looks;
}

std::optional<FilmGrainLook> film_grain_look(std::string_view name)
{
    for (const FilmGrainLook &look : film_grain_looks()) {
        if (look.name == name) {
            return look;
        }
    }
    return std::nullopt;
}

} // namespace genesis::render
