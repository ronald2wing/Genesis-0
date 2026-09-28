// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Clip model: the kinds a clip can be and one placed piece of a timeline.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "core/Animation.h"
#include "core/ClipTimeline.h"

#include "project/model/Cutout.h"
#include "project/model/Effects.h"
#include "project/model/Keyframe.h"
#include "project/model/Speed.h"
#include "project/model/Text.h"

namespace genesis::project {

using genesis::core::Rational;

// What a clip can be - wider than MediaKind because a text clip has no
// file behind it; it is its own content.
// On disk: lowercase.
enum class ClipKind {
    // A cut of a video media item.
    Video,
    // A cut of audio - either audio media or sound detached from a video.
    Audio,
    // A placed still.
    Image,
    // A title. No media behind it; the content lives in Clip::text.
    Text,
    // A treatment over everything beneath it for as long as it runs - a
    // look or an effect placed as a layer. No media behind it; the chain
    // lives in Clip::video_effects, its strength in Clip::opacity
    // and its ramps in the fades.
    Layer,
};

// One placed piece of a timeline: a stretch of media (or a title) with its
// timing, mix, transform, and effects. Everything an edit decision touches
// lives here, which is why most commands are clip commands.
// On disk: camelCase.
struct Clip
{
    // Minted by the editor ("c1", "c2", ...); how every command names its
    // target. Survives splits - the head keeps the id - and merges.
    std::string id;
    // The lane this clip sits on. Always a track of the same timeline; the
    // reader drops clips whose track vanished.
    std::string track_id;
    // Empty for a text clip, which has no file behind it.
    std::string media_id;
    // Display label. Snapshotted from the media's name (or a title's first
    // line) at creation, then the user's to change.
    std::string name = "clip";
    // What this clip renders as. Follows the media's kind, and is rewritten
    // when a template slot is filled with a different kind of file.
    ClipKind kind = ClipKind::Video;
    // Seconds from the start of the timeline.
    Rational start = Rational::ZERO;
    // Seconds of timeline the clip occupies. Source seconds divided by
    // `speed`; never below a sixtieth of a second.
    Rational duration = Rational::ONE;
    // In-point: how far into the media the clip starts.
    Rational source_start = Rational::ZERO;
    // Linear gain, 1 being unity.
    double volume = 1.0;
    // Stereo pan, -1 full left, +1 full right; 0 centred. A constant, not
    // a keyable ride - the pan knob is a single value for the whole clip.
    double pan = 0.0;
    // Seconds of audio ramp-in from silence at the head. Zero for none.
    Rational fade_in = Rational::ZERO;
    // Seconds of audio ramp-out to silence at the tail. Zero for none.
    Rational fade_out = Rational::ZERO;
    // Multiplier over the fitted size. 1 fills the frame, preserving aspect.
    double scale = 1.0;
    // Offset from centred, as a fraction of frame width / height.
    double offset_x = 0.0;
    // The vertical half of `offset_x`'s pair; positive moves down.
    double offset_y = 0.0;
    // Clockwise rotation in degrees, about the picture's centre.
    double rotation = 0.0;
    // A multiplier on the fitted width beyond `scale`, for a picture
    // pulled wider or narrower than its aspect; 1 keeps the aspect.
    double stretch_x = 1.0;
    // The same for the height.
    double stretch_y = 1.0;
    // Blend strength over whatever is beneath, in 0..1.
    double opacity = 1.0;
    // Playback rate. 1 is normal. With a curve set this is the curve's
    // mean, kept in step by the commands that set either.
    double speed = 1.0;
    // Speed as it changes over the clip: points of `(at, speed)`, `at` a
    // fraction of the clip's timeline length. None is the constant rate.
    std::optional<std::vector<SpeedPoint>> speed_curve;
    // The user's own keys, sorted by property and then by `at`. Empty is a
    // clip whose properties are the constants above. A keyed property's
    // keys travel absolutely: they replace the constant rather than ride
    // on it.
    std::vector<ClipKey> keys;
    // Mirrored left to right.
    bool flip_h = false;
    // Mirrored top to bottom.
    bool flip_v = false;
    // Played backwards: the engine shows the covered source span in reverse.
    // A playback direction, not a transform - the clip keeps its length,
    // in-point and looks, and the app generates a reversed copy of the
    // source span when it builds the timeline.
    bool reverse = false;
    // How the picture's colour meets what is beneath it: "normal",
    // "multiply", "screen", "add", "lighten", "darken".
    std::string blend;
    // What is cut off each edge of the source before it is fitted, as
    // fractions of the source's width and height.
    std::optional<Crop> crop;
    // The background taken away by a mask rather than a key colour; see
    // `Cutout`. Keying by colour is a package on `video_effects`.
    std::optional<Cutout> cutout;
    // Keep voices at their natural pitch when `speed` is not 1. On by
    // default; off gives the tape-machine chipmunk/slow-motion sound.
    bool preserve_pitch = true;
    // Audio filters, in order - order is audible.
    std::vector<AppliedFilter> filters;
    // Video effects, in order - the visual sibling of `filters`.
    std::vector<AppliedFilter> video_effects;
    // Which of the media's audio streams this clip plays, by the stream's
    // index in the file - see `MediaItem::audio_tracks`. None is the first
    // in file order, which is every clip from before files with several
    // were told apart and every clip of a file with one.
    std::optional<std::uint32_t> audio_stream;
    // True when a video clip's embedded audio is detached out of it.
    std::optional<bool> muted;
    // On a detached audio clip: the video clip the sound came from.
    std::optional<std::string> detached_from;
    // The transition on the cut into this clip, if any.
    std::optional<Transition> transition_in;
    // The overlay, when this is a text clip.
    std::optional<TextStyle> text;

    // What a document entry starts from before its own fields land on
    // it: `Clip::blank` with no id, on no track, one second of video.
    Clip() = default;

    bool operator==(const Clip &) const = default;

    // A clip with nothing set but what names and places it: unity gain,
    // scale and speed, no fades, no effects, no keys. Every constructor
    // starts here and sets the few fields its kind needs, so a field added
    // to the model is added in one place.
    static Clip blank(std::string id, std::string track_id, ClipKind kind, std::string name,
                      Rational start, Rational duration)
    {
        const Rational MIN_CLIP_DURATION = Rational{ 1, 60 };
        Clip clip;
        clip.id = std::move(id);
        clip.track_id = std::move(track_id);
        clip.name = std::move(name);
        clip.kind = kind;
        clip.start = std::max(start, Rational::ZERO);
        clip.duration = std::max(duration, MIN_CLIP_DURATION);
        return clip;
    }

    // Pulls every number back into the range a command would have held
    // it to: the floors and ceilings in `ranges`, the rotation wrapped,
    // non-finite values replaced by their neutral, the crop and the cutout
    // tidied, the keys sorted. What the document reader runs over a clip
    // it has just read, so a hand-edited or older file holds nothing an
    // edit could not have produced.
    Clip tidy() const
    {
        const Rational MIN_CLIP_DURATION = Rational{ 1, 60 };
        constexpr double MIN_SPEED = 0.0625;
        constexpr double MAX_SPEED = 16.0;
        constexpr double MIN_SCALE = 0.05;
        constexpr double MAX_SCALE = 8.0;
        constexpr double MIN_STRETCH = 0.1;
        constexpr double MAX_STRETCH = 10.0;
        constexpr double MAX_OFFSET = 3.0;
        const Rational MIN_TRANSITION = Rational{ 1, 10 };
        const auto finite = [](double value, double fallback) {
            return std::isfinite(value) ? value : fallback;
        };
        const auto wrap_rotation = [](double degrees) {
            const double wrapped = std::fmod(std::fmod(degrees, 360.0) + 540.0, 360.0) - 180.0;
            return wrapped == -180.0 ? 180.0 : wrapped;
        };
        Clip out = *this;
        out.start = std::max(out.start, Rational::ZERO);
        out.duration = std::max(out.duration, MIN_CLIP_DURATION);
        out.source_start = std::max(out.source_start, Rational::ZERO);
        out.volume = std::max(finite(out.volume, 1.0), 0.0);
        out.pan = std::clamp(finite(out.pan, 0.0), -1.0, 1.0);
        out.fade_in = std::max(out.fade_in, Rational::ZERO);
        out.fade_out = std::max(out.fade_out, Rational::ZERO);
        out.scale = std::clamp(finite(out.scale, 1.0), MIN_SCALE, MAX_SCALE);
        out.offset_x = std::clamp(finite(out.offset_x, 0.0), -MAX_OFFSET, MAX_OFFSET);
        out.offset_y = std::clamp(finite(out.offset_y, 0.0), -MAX_OFFSET, MAX_OFFSET);
        out.rotation = wrap_rotation(finite(out.rotation, 0.0));
        out.stretch_x = std::clamp(finite(out.stretch_x, 1.0), MIN_STRETCH, MAX_STRETCH);
        out.stretch_y = std::clamp(finite(out.stretch_y, 1.0), MIN_STRETCH, MAX_STRETCH);
        out.opacity = std::clamp(finite(out.opacity, 1.0), 0.0, 1.0);
        out.speed = std::clamp(finite(out.speed, 1.0), MIN_SPEED, MAX_SPEED);
        if (out.speed_curve) {
            std::vector<SpeedPoint> points;
            points.reserve(out.speed_curve->size());
            for (const SpeedPoint &point : *out.speed_curve) {
                if (std::isfinite(point.at) && point.at >= 0.0 && point.at <= 1.0) {
                    points.push_back(SpeedPoint{
                            point.at,
                            std::clamp(finite(point.speed, 1.0), MIN_SPEED, MAX_SPEED),
                    });
                }
            }
            if (points.empty()) {
                out.speed_curve.reset();
            } else {
                out.speed_curve = std::move(points);
            }
        }
        if (out.crop) {
            Crop tidied = out.crop->tidy();
            if (tidied.is_none()) {
                out.crop.reset();
            } else {
                out.crop = std::move(tidied);
            }
        }
        if (out.cutout) {
            out.cutout = out.cutout->tidy();
        }
        if (out.transition_in) {
            out.transition_in->duration = std::max(out.transition_in->duration, MIN_TRANSITION);
        }
        // A key holds what the field itself may hold. Rotation is the one
        // exception: a key past a full turn is a spin, and wrapping it
        // would take the spin away.
        for (ClipKey &key : out.keys) {
            switch (key.property) {
            case KeyProperty::Scale:
                key.value = std::clamp(key.value, MIN_SCALE, MAX_SCALE);
                break;
            case KeyProperty::Opacity:
                key.value = std::clamp(key.value, 0.0, 1.0);
                break;
            case KeyProperty::Volume:
                key.value = std::max(key.value, 0.0);
                break;
            case KeyProperty::OffsetX:
            case KeyProperty::OffsetY:
                key.value = std::clamp(key.value, -MAX_OFFSET, MAX_OFFSET);
                break;
            case KeyProperty::Rotation:
                break;
            }
        }
        out.sort_keys();
        for (std::vector<AppliedFilter> *chain : { &out.filters, &out.video_effects }) {
            for (AppliedFilter &entry : *chain) {
                entry.sort_keys();
            }
        }
        if (out.text) {
            out.text = out.text->tidy();
        }
        if (out.muted && *out.muted == false) {
            out.muted.reset();
        }
        return out;
    }

    // The clip's own constant for a keyable property - what the property is
    // worth everywhere its track is silent.
    double constant(KeyProperty property) const
    {
        switch (property) {
        case KeyProperty::Scale:
            return scale;
        case KeyProperty::OffsetX:
            return offset_x;
        case KeyProperty::OffsetY:
            return offset_y;
        case KeyProperty::Rotation:
            return rotation;
        case KeyProperty::Opacity:
            return opacity;
        case KeyProperty::Volume:
            return volume;
        }
        return scale;
    }

    // This property's keys, in order. Borrowed rather than collected: the
    // caller is usually asking a question about them, not keeping them.
    std::span<const ClipKey> keys_on(KeyProperty property) const
    {
        const auto first = std::find_if(keys.begin(), keys.end(), [property](const ClipKey &key) {
            return key.property == property;
        });
        if (first == keys.end()) {
            return { };
        }
        const auto last = std::find_if(first, keys.end(), [property](const ClipKey &key) {
            return key.property != property;
        });
        return std::span<const ClipKey>(&*first, static_cast<std::size_t>(last - first));
    }

    // Whether this property is keyed at all. A property with keys is one
    // the panel shows as animated, however few there are.
    bool is_keyed(KeyProperty property) const { return !keys_on(property).empty(); }

    // The index into `keys` of this property's key at `at`, within
    // `KEY_EPSILON`, choosing the nearest when two are in reach.
    std::optional<std::size_t> key_at(KeyProperty property, double at) const
    {
        std::optional<std::size_t> best;
        double best_dist = 0.0;
        for (std::size_t index = 0; index < keys.size(); ++index) {
            const ClipKey &key = keys[index];
            if (key.property != property) {
                continue;
            }
            const double dist = std::abs(key.at - at);
            if (dist > KEY_EPSILON) {
                continue;
            }
            if (!best || dist < best_dist) {
                best = index;
                best_dist = dist;
            }
        }
        return best;
    }

    // This property's keys as the engine plays them.
    genesis::core::KeyTrack track_on(KeyProperty property) const
    {
        std::vector<genesis::core::Key> held_keys;
        for (const ClipKey &held : keys_on(property)) {
            held_keys.push_back(genesis::core::Key{
                    held.at,
                    held.value,
                    static_cast<genesis::core::Ease>(held.ease),
            });
        }
        return genesis::core::KeyTrack(std::move(held_keys));
    }

    // What a property is worth at `at`: its ride where it is keyed, and its
    // constant where it is not.
    //
    // The same arithmetic the engine does, which is what makes a key put on
    // where the ride already is change nothing on screen - and what lets a
    // panel show the live value under the playhead without asking the
    // renderer.
    double value_at(KeyProperty property, double at) const
    {
        const double constant_value = constant(property);
        if (!is_keyed(property)) {
            return constant_value;
        }
        return track_on(property).value_at(at, constant_value);
    }

    // Sets a key, replacing whichever key on that property was already
    // within `KEY_EPSILON` of `at`. Keeps `keys` sorted by property and
    // then by `at`, which is what lets everything else read them in order
    // without sorting first.
    void set_key(KeyProperty property, double at, double value, KeyEase ease)
    {
        if (!std::isfinite(at) || !std::isfinite(value)) {
            return;
        }
        at = std::clamp(at, 0.0, 1.0);
        const std::optional<std::size_t> slot = key_at(property, at);
        if (slot) {
            keys[*slot] = ClipKey{ property, at, value, ease };
        } else {
            keys.push_back(ClipKey{ property, at, value, ease });
        }
        sort_keys();
    }

    // Removes this property's key at `at`, if there is one. True when a key
    // actually went.
    bool clear_key(KeyProperty property, double at)
    {
        const std::optional<std::size_t> index = key_at(property, at);
        if (!index) {
            return false;
        }
        keys.erase(keys.begin() + *index);
        return true;
    }

    // Takes every key off one property. True when there were any.
    bool clear_keys(KeyProperty property)
    {
        const std::size_t before = keys.size();
        std::erase_if(keys, [property](const ClipKey &key) { return key.property == property; });
        return keys.size() != before;
    }

    // Re-anchors every key - the clip's own and its effects' - after the
    // clip's span changed, so each key stays at the instant of the picture
    // it was set on.
    //
    // Keys are stored as fractions of the clip's length, which is the
    // right unit for a preset and the wrong one for an edit: a split, a
    // trim or a merge changes the length under them, and without this a
    // fade over the first half of a clip becomes a fade over the first
    // half of each piece. `old` is the length the keys were set against;
    // `[from, to]` is the window of that length the clip now covers, in
    // seconds from its old start. A window past either end of the old
    // clip (a trim that extends it, a merge) simply spreads the keys over
    // the longer span, where the ride holds its end values anyway.
    //
    // A key outside the window is replaced by one on the window's edge
    // carrying the ride's value there, so the piece plays exactly what
    // the whole played over that stretch, and a merge of the pieces gets
    // the ride back.
    void rewindow_keys(Rational old, Rational from, Rational to)
    {
        const bool sane = old > Rational::ZERO && to > from;
        if (!sane) {
            return;
        }
        // fraction conversion: keys are stored as double fractions, so the
        // rational window converts to fractions once, here.
        const double a = (from / old).as_double();
        const double b = (to / old).as_double();
        auto rewindow = [](const auto &held, double a, double b,
                           auto value_at) -> std::vector<std::tuple<double, double, KeyEase>> {
            const double span = b - a;
            std::vector<std::tuple<double, double, KeyEase>> out;
            out.reserve(held.size() + 2);
            const bool before = std::any_of(held.begin(), held.end(), [a](const auto &key) {
                return key.at < a - KEY_EPSILON;
            });
            if (before) {
                out.emplace_back(0.0, value_at(a), KeyEase::linear());
            }
            std::optional<KeyEase> first_after;
            for (const auto &key : held) {
                if (key.at < a - KEY_EPSILON) {
                    continue;
                }
                if (key.at > b + KEY_EPSILON) {
                    if (!first_after) {
                        first_after = key.ease;
                    }
                    continue;
                }
                out.emplace_back(std::clamp((key.at - a) / span, 0.0, 1.0), key.value, key.ease);
            }
            if (first_after) {
                out.emplace_back(1.0, value_at(b), *first_after);
            }
            return out;
        };
        for (KeyProperty property : key_property_all) {
            if (!is_keyed(property)) {
                continue;
            }
            const genesis::core::KeyTrack ride = track_on(property);
            const double constant_value = constant(property);
            const auto windowed = rewindow(keys_on(property), a, b, [&](double x) {
                return ride.value_at(x, constant_value);
            });
            std::erase_if(keys,
                          [property](const ClipKey &key) { return key.property == property; });
            for (const auto &[at, value, ease] : windowed) {
                keys.push_back(ClipKey{ property, at, value, ease });
            }
        }
        for (std::vector<AppliedFilter> *chain : { &filters, &video_effects }) {
            for (AppliedFilter &link : *chain) {
                std::vector<std::string> names;
                names.reserve(link.keys.size());
                for (const auto &[name, held] : link.keys) {
                    names.push_back(name);
                }
                for (const std::string &name : names) {
                    const genesis::core::KeyTrack ride = link.track_on(name);
                    const auto it = link.params.find(name);
                    const double constant_value = it != link.params.end() ? it->second : 0.0;
                    const auto windowed = rewindow(link.keys_on(name), a, b, [&](double x) {
                        return ride.value_at(x, constant_value);
                    });
                    std::vector<ParamKey> run;
                    run.reserve(windowed.size());
                    for (const auto &[at, value, ease] : windowed) {
                        run.push_back(ParamKey{ at, value, ease });
                    }
                    link.keys[name] = std::move(run);
                }
                link.sort_keys();
            }
        }
        sort_keys();
    }

    // Takes on the keys of `piece`, a clip that sat `offset` seconds
    // after this one's start and has been merged into it: each of its
    // keys lands at the same instant of the picture it marked, now
    // measured over this clip's `duration`. Effect keys come across where
    // the effect at the same position of the chain is the same effect.
    void absorb_keys(const Clip &piece, Rational offset)
    {
        const bool sane = duration > Rational::ZERO;
        if (!sane) {
            return;
        }
        // fraction conversion: keys are stored as double fractions, so the
        // rational lengths and offset convert to doubles once, here.
        const double dur = duration.as_double();
        const double piece_dur = piece.duration.as_double();
        const double off = offset.as_double();
        for (const ClipKey &key : piece.keys) {
            const double at = (off + (key.at * piece_dur)) / dur;
            set_key(key.property, at, key.value, key.ease);
        }
        const auto absorb_chain = [&](std::vector<AppliedFilter> &mine,
                                      const std::vector<AppliedFilter> &theirs) {
            const std::size_t count = std::min(mine.size(), theirs.size());
            for (std::size_t i = 0; i < count; ++i) {
                if (mine[i].id != theirs[i].id) {
                    continue;
                }
                for (const auto &[name, run] : theirs[i].keys) {
                    for (const ParamKey &key : run) {
                        const double at = (off + (key.at * piece_dur)) / dur;
                        mine[i].set_key(name, at, key.value, key.ease);
                    }
                }
            }
        };
        absorb_chain(filters, piece.filters);
        absorb_chain(video_effects, piece.video_effects);
    }

    // Drops keys that are not finite or not in `0..=1`, then orders them.
    // Called by everything that can put a key in, including the document
    // reader, so a hand-edited file cannot produce an unsorted track.
    void sort_keys()
    {
        std::erase_if(keys, [](const ClipKey &key) {
            return !std::isfinite(key.at) || !std::isfinite(key.value) || key.at < 0.0
                    || key.at > 1.0;
        });
        std::sort(keys.begin(), keys.end(), [](const ClipKey &a, const ClipKey &b) {
            if (a.property != b.property) {
                return static_cast<int>(a.property) < static_cast<int>(b.property);
            }
            return a.at < b.at;
        });
    }
};

} // namespace genesis::project
