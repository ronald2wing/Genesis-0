// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "project/commands/Apply.h"
#include "project/Naming.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "core/SpeedCurve.h"

namespace genesis::project {
namespace {

// The floor on a clip's timeline length: a sixtieth of a second.
const Rational MIN_CLIP_DURATION = Rational{ 1, 60 };

// What the transform fields clamp to, matching `ranges` in the Rust model.
constexpr double MIN_SCALE = 0.05;
constexpr double MAX_SCALE = 8.0;
constexpr double MAX_OFFSET = 3.0;
constexpr double MIN_STRETCH = 0.1;
constexpr double MAX_STRETCH = 10.0;

// A rotation kept in (-180, 180] so a full drag never accumulates turns.
double wrap_rotation(double degrees)
{
    const double wrapped = std::fmod(std::fmod(degrees, 360.0) + 540.0, 360.0) - 180.0;
    return wrapped == -180.0 ? 180.0 : wrapped;
}

// The engine's curve for these points, or nullopt when they make no curve.
std::optional<genesis::core::SpeedCurve> curve_of(const std::vector<SpeedPoint> &points)
{
    std::vector<std::pair<double, double>> raw;
    raw.reserve(points.size());
    for (const SpeedPoint &point : points) {
        raw.emplace_back(point.at, point.speed);
    }
    return genesis::core::SpeedCurve::create(raw);
}

// Source seconds per timeline second over a whole clip with these points;
// the constant rate's equivalent for a curve.
double mean_of(const std::vector<SpeedPoint> &points)
{
    const std::optional<genesis::core::SpeedCurve> curve = curve_of(points);
    return curve ? curve->mean() : 1.0;
}

// The link a key command targets: the `entry`-th video effect on the clip.
// Records the clip's before-image, then hands the link back; null when the
// clip is unknown or the entry is out of range, which every such command
// treats as a tolerated no-op.
AppliedFilter *effect_link(UndoRecord &undo, Timeline &timeline, std::string_view clip_id,
                           std::size_t entry)
{
    Clip *clip = timeline.clip_mut(clip_id);
    if (clip == nullptr || entry >= clip->video_effects.size()) {
        return nullptr;
    }
    record_before(undo, *clip);
    return &clip->video_effects[entry];
}

} // namespace

// What a placed clip looks and sounds like: its patchable fields, speed,
// transform, keys and cutout.
//
// One arm per command, exactly as `apply` routes them here; everything these
// arms share lives in the parent module.
ApplyResult apply_properties(const Command &command, Project &project,
                             [[maybe_unused]] IdMinter &mint, UndoRecord &undo)
{
    return std::visit(
            [&](const auto &alternative) -> ApplyResult {
                using T = std::decay_t<decltype(alternative)>;

                if constexpr (std::is_same_v<T, UpdateClip>) {
                    // Applies a ClipPatch: only the fields present change, each
                    // with the clamp documented on it. An unknown clip is a
                    // no-op.
                    Timeline &timeline = project.active_mut();
                    Clip *clip = timeline.clip_mut(alternative.clip_id);
                    if (clip == nullptr) {
                        return Outcome{ std::nullopt, false };
                    }
                    record_before(undo, *clip);
                    const ClipPatch &patch = alternative.patch;
                    bool applied = false;
                    if (patch.name) {
                        applied |= assign(clip->name, *patch.name);
                    }
                    if (patch.volume) {
                        applied |= assign(clip->volume, std::max(*patch.volume, 0.0));
                    }
                    if (patch.fade_in) {
                        applied |= assign(clip->fade_in, std::max(*patch.fade_in, Rational::ZERO));
                    }
                    if (patch.fade_out) {
                        applied |=
                                assign(clip->fade_out, std::max(*patch.fade_out, Rational::ZERO));
                    }
                    if (patch.opacity) {
                        applied |= assign(clip->opacity, std::clamp(*patch.opacity, 0.0, 1.0));
                    }
                    if (patch.preserve_pitch) {
                        applied |= assign(clip->preserve_pitch, *patch.preserve_pitch);
                    }
                    if (patch.muted) {
                        // Unmuted is the absent value, so a document never carries
                        // a `muted: false` that means the same as nothing.
                        applied |= assign(clip->muted,
                                          *patch.muted ? std::optional<bool>{ true }
                                                       : std::optional<bool>{ });
                    }
                    if (patch.flip_h) {
                        applied |= assign(clip->flip_h, *patch.flip_h);
                    }
                    if (patch.flip_v) {
                        applied |= assign(clip->flip_v, *patch.flip_v);
                    }
                    if (patch.blend) {
                        const std::string blend =
                                *patch.blend == "normal" ? std::string{ } : *patch.blend;
                        applied |= assign(clip->blend, blend);
                    }
                    if (patch.crop) {
                        // Absent leaves it alone, null takes it off, a value
                        // replaces it - tidied, and dropped when nothing is left
                        // cut.
                        std::optional<Crop> crop;
                        if (*patch.crop) {
                            const Crop tidied = (*patch.crop)->tidy();
                            if (!tidied.is_none()) {
                                crop = tidied;
                            }
                        }
                        applied |= assign(clip->crop, crop);
                    }
                    if (patch.filters) {
                        applied |= assign(clip->filters, *patch.filters);
                    }
                    if (patch.video_effects) {
                        applied |= assign(clip->video_effects, *patch.video_effects);
                    }
                    if (patch.transition_in) {
                        applied |= assign(clip->transition_in, *patch.transition_in);
                    }
                    if (patch.text) {
                        // The name follows the words, like addTextClip snapshots
                        // it.
                        if (*patch.text) {
                            applied |= assign(clip->name, first_line((*patch.text)->content));
                        }
                        applied |= assign(clip->text, *patch.text);
                    }
                    if (patch.audio_stream) {
                        applied |= assign(clip->audio_stream, *patch.audio_stream);
                    }
                    return Outcome{ std::nullopt, applied };
                }

                if constexpr (std::is_same_v<T, SetClipSpeed>) {
                    // Changes playback rate while holding the amount of source
                    // covered constant, so the timeline length stretches rather
                    // than the clip trimming. An unknown clip is a no-op.
                    Timeline &timeline = project.active_mut();
                    Clip *clip = timeline.clip_mut(alternative.clip_id);
                    if (clip == nullptr) {
                        return Outcome{ std::nullopt, false };
                    }
                    record_before(undo, *clip);
                    // The amount of source covered is held constant - that is
                    // what makes this a speed change rather than a trim.
                    const double next =
                            std::clamp(alternative.speed, genesis::core::SpeedCurve::MIN_SPEED,
                                       genesis::core::SpeedCurve::MAX_SPEED);
                    const Rational source_covered =
                            clip->duration * *Rational::approximate(clip->speed);
                    // Bitwise so no assignment is short-circuited away. A rate
                    // set by hand is a constant rate: the curve goes.
                    bool applied = assign(clip->speed, next);
                    applied |= assign(clip->duration,
                                      std::max(source_covered / *Rational::approximate(next),
                                               MIN_CLIP_DURATION));
                    applied |= assign(clip->speed_curve, std::optional<std::vector<SpeedPoint>>{ });
                    return Outcome{ std::nullopt, applied };
                }

                if constexpr (std::is_same_v<T, SetClipReverse>) {
                    // Turns a clip's playback backwards. A playback direction,
                    // not a transform: the clip keeps its length, in-point and
                    // looks, and the app generates a reversed copy of the source
                    // span when it builds the timeline. An unknown clip is a
                    // no-op.
                    Timeline &timeline = project.active_mut();
                    Clip *clip = timeline.clip_mut(alternative.clip_id);
                    if (clip == nullptr) {
                        return Outcome{ std::nullopt, false };
                    }
                    record_before(undo, *clip);
                    const bool applied = assign(clip->reverse, alternative.reverse);
                    return Outcome{ std::nullopt, applied };
                }

                if constexpr (std::is_same_v<T, SetClipPan>) {
                    // Moves a clip's sound across the stereo field. A constant,
                    // not a keyable ride; the value is clamped to the field's
                    // range on the way in. An unknown clip is a no-op.
                    Timeline &timeline = project.active_mut();
                    Clip *clip = timeline.clip_mut(alternative.clip_id);
                    if (clip == nullptr) {
                        return Outcome{ std::nullopt, false };
                    }
                    record_before(undo, *clip);
                    const bool applied = assign(clip->pan, std::clamp(alternative.pan, -1.0, 1.0));
                    return Outcome{ std::nullopt, applied };
                }

                if constexpr (std::is_same_v<T, SetClipCutout>) {
                    // Sets or clears a picture's cutout, tidying it on the way
                    // in; see Cutout::tidy. An unknown clip is a no-op.
                    Timeline &timeline = project.active_mut();
                    Clip *clip = timeline.clip_mut(alternative.clip_id);
                    if (clip == nullptr) {
                        return Outcome{ std::nullopt, false };
                    }
                    record_before(undo, *clip);
                    std::optional<Cutout> tidied;
                    if (alternative.cutout) {
                        tidied = alternative.cutout->tidy();
                    }
                    const bool applied = assign(clip->cutout, tidied);
                    return Outcome{ std::nullopt, applied };
                }

                if constexpr (std::is_same_v<T, AddCutoutStroke>) {
                    // Paints one stroke onto a clip's cutout: a clip with no
                    // cutout gets a custom one, and an automatic cutout becomes
                    // custom. A stroke with no points is a no-op.
                    Timeline &timeline = project.active_mut();
                    Clip *clip = timeline.clip_mut(alternative.clip_id);
                    if (clip == nullptr) {
                        return Outcome{ std::nullopt, false };
                    }
                    std::optional<Stroke> stroke = alternative.stroke.tidy();
                    if (!stroke) {
                        return Outcome{ std::nullopt, false };
                    }
                    record_before(undo, *clip);
                    if (!clip->cutout) {
                        clip->cutout = Cutout::automatic();
                    }
                    Cutout &cutout = *clip->cutout;
                    cutout.mode = CutoutMode::Custom;
                    cutout.strokes.push_back(std::move(*stroke));
                    return Outcome{ std::nullopt, true };
                }

                if constexpr (std::is_same_v<T, SetClipKey>) {
                    // Puts a key on one property of a clip, replacing whichever
                    // key on that property was already within a hair of it. An
                    // unknown clip is a no-op.
                    Timeline &timeline = project.active_mut();
                    Clip *clip = timeline.clip_mut(alternative.clip_id);
                    if (clip == nullptr) {
                        return Outcome{ std::nullopt, false };
                    }
                    // Clamped the way the field itself is, so a key can never
                    // hold a value the constant would have been refused.
                    double value = alternative.value;
                    switch (alternative.property) {
                    case KeyProperty::Scale:
                        value = std::clamp(value, MIN_SCALE, MAX_SCALE);
                        break;
                    case KeyProperty::Opacity:
                        value = std::clamp(value, 0.0, 1.0);
                        break;
                    case KeyProperty::Volume:
                        // No ceiling, matching ClipPatch: the level fader
                        // goes to +24 dB because quiet material needs it,
                        // and a key is the same value at a different
                        // instant.
                        value = std::max(value, 0.0);
                        break;
                    case KeyProperty::OffsetX:
                    case KeyProperty::OffsetY:
                    case KeyProperty::Rotation:
                        break;
                    }
                    record_before(undo, *clip);
                    const std::vector<ClipKey> before = clip->keys;
                    clip->set_key(alternative.property, alternative.at, value, alternative.ease);
                    return Outcome{ std::nullopt, clip->keys != before };
                }

                if constexpr (std::is_same_v<T, ClearClipKey>) {
                    // Takes the key at `at` off one property, if there is one
                    // there. An unknown clip is a no-op.
                    Timeline &timeline = project.active_mut();
                    Clip *clip = timeline.clip_mut(alternative.clip_id);
                    if (clip == nullptr) {
                        return Outcome{ std::nullopt, false };
                    }
                    record_before(undo, *clip);
                    const bool applied = clip->clear_key(alternative.property, alternative.at);
                    return Outcome{ std::nullopt, applied };
                }

                if constexpr (std::is_same_v<T, ClearClipKeys>) {
                    // Takes every key off one property, returning it to its
                    // constant. An unknown clip is a no-op.
                    Timeline &timeline = project.active_mut();
                    Clip *clip = timeline.clip_mut(alternative.clip_id);
                    if (clip == nullptr) {
                        return Outcome{ std::nullopt, false };
                    }
                    record_before(undo, *clip);
                    const bool applied = clip->clear_keys(alternative.property);
                    return Outcome{ std::nullopt, applied };
                }

                if constexpr (std::is_same_v<T, SetEffectKey>) {
                    // Puts a key on one parameter of one link of a clip's
                    // picture chain, replacing whichever key on it was already
                    // within a hair of `at`. An unknown clip or an out-of-range
                    // link is a no-op.
                    Timeline &timeline = project.active_mut();
                    AppliedFilter *link =
                            effect_link(undo, timeline, alternative.clip_id, alternative.entry);
                    if (link == nullptr) {
                        return Outcome{ std::nullopt, false };
                    }
                    const auto before = link->keys;
                    link->set_key(alternative.key, alternative.at, alternative.value,
                                  alternative.ease);
                    return Outcome{ std::nullopt, link->keys != before };
                }

                if constexpr (std::is_same_v<T, ClearEffectKey>) {
                    // Takes the key at `at` off one parameter of one link, if
                    // there is one. An unknown clip or an out-of-range link is
                    // a no-op.
                    Timeline &timeline = project.active_mut();
                    AppliedFilter *link =
                            effect_link(undo, timeline, alternative.clip_id, alternative.entry);
                    if (link == nullptr) {
                        return Outcome{ std::nullopt, false };
                    }
                    const bool applied = link->clear_key(alternative.key, alternative.at);
                    return Outcome{ std::nullopt, applied };
                }

                if constexpr (std::is_same_v<T, ClearEffectKeys>) {
                    // Takes every key off one parameter of one link, returning
                    // it to the value it holds. An unknown clip or an
                    // out-of-range link is a no-op.
                    Timeline &timeline = project.active_mut();
                    AppliedFilter *link =
                            effect_link(undo, timeline, alternative.clip_id, alternative.entry);
                    if (link == nullptr) {
                        return Outcome{ std::nullopt, false };
                    }
                    const bool applied = link->clear_keys(alternative.key);
                    return Outcome{ std::nullopt, applied };
                }

                if constexpr (std::is_same_v<T, SetClipSpeedCurve>) {
                    // Gives a clip a speed curve, or takes it away. The source
                    // covered is held constant, as a speed change does, so the
                    // clip's timeline length follows the curve's mean; `speed`
                    // is kept at that mean. An unknown clip is a no-op.
                    Timeline &timeline = project.active_mut();
                    Clip *clip = timeline.clip_mut(alternative.clip_id);
                    if (clip == nullptr) {
                        return Outcome{ std::nullopt, false };
                    }
                    // Points that make no curve are the same as none, so the
                    // curve is dropped on the way in.
                    std::optional<std::vector<SpeedPoint>> curve = alternative.curve;
                    if (curve && !curve_of(*curve)) {
                        curve.reset();
                    }
                    record_before(undo, *clip);
                    const Rational source_covered =
                            clip->duration * *Rational::approximate(clip->speed);
                    const double mean = std::clamp(curve ? mean_of(*curve) : clip->speed,
                                                   genesis::core::SpeedCurve::MIN_SPEED,
                                                   genesis::core::SpeedCurve::MAX_SPEED);
                    bool applied = assign(clip->speed_curve, curve);
                    applied |= assign(clip->speed, mean);
                    applied |= assign(clip->duration,
                                      std::max(source_covered / *Rational::approximate(mean),
                                               MIN_CLIP_DURATION));
                    return Outcome{ std::nullopt, applied };
                }

                if constexpr (std::is_same_v<T, SetClipTransform>) {
                    // Adjusts the picture's placement. Each field is optional so
                    // a drag can send just the axis it moved; absent fields stay
                    // put. An unknown clip is a no-op.
                    Timeline &timeline = project.active_mut();
                    Clip *clip = timeline.clip_mut(alternative.clip_id);
                    if (clip == nullptr) {
                        return Outcome{ std::nullopt, false };
                    }
                    record_before(undo, *clip);
                    bool applied = false;
                    if (alternative.scale) {
                        applied |= assign(clip->scale,
                                          std::clamp(*alternative.scale, MIN_SCALE, MAX_SCALE));
                    }
                    if (alternative.offset_x) {
                        applied |=
                                assign(clip->offset_x,
                                       std::clamp(*alternative.offset_x, -MAX_OFFSET, MAX_OFFSET));
                    }
                    if (alternative.offset_y) {
                        applied |=
                                assign(clip->offset_y,
                                       std::clamp(*alternative.offset_y, -MAX_OFFSET, MAX_OFFSET));
                    }
                    if (alternative.rotation) {
                        applied |= assign(clip->rotation, wrap_rotation(*alternative.rotation));
                    }
                    if (alternative.stretch_x) {
                        applied |= assign(
                                clip->stretch_x,
                                std::clamp(*alternative.stretch_x, MIN_STRETCH, MAX_STRETCH));
                    }
                    if (alternative.stretch_y) {
                        applied |= assign(
                                clip->stretch_y,
                                std::clamp(*alternative.stretch_y, MIN_STRETCH, MAX_STRETCH));
                    }
                    return Outcome{ std::nullopt, applied };
                }

                // `apply` routes only this module's commands here.
                std::unreachable();
            },
            command.value);
}

} // namespace genesis::project
