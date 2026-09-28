// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Re-derived from Concat's concat-core (time.rs, arena.rs, timeline.rs),
// GPL-3.0-or-later, Copyright (C) 2026 Jareer and Concat contributors.
// This is a C++23 re-derivation, not a transliteration; see
// docs/rewrite-plan.md section 1.

#ifndef GENESIS0_CORE_CLIP_TIMELINE_H
#define GENESIS0_CORE_CLIP_TIMELINE_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace genesis::core {

// ---------------------------------------------------------------------------
// Rational time
// ---------------------------------------------------------------------------
//
// Video editing is frame-accurate arithmetic on rates that are not
// representable in binary floating point. 29.97 fps is really 30000/1001, and
// a double accumulator drifts: after an hour of adding 1.0/29.97 you are
// visibly off. Every timestamp here is an exact rational number of seconds;
// conversion to double happens only at the edges (display, engine arguments).

class Rational
{
public:
    static const Rational ZERO;
    static const Rational ONE;

    // Builds num/den in lowest terms with a positive denominator.
    // Precondition: den != 0.
    Rational(std::int64_t num, std::int64_t den);

    static Rational from_int(std::int64_t value) { return Rational(value, 1); }

    // Parses FFmpeg's "num/den" fraction syntax, also accepting a bare integer
    // or decimal. Returns nullopt on anything else, including "0/0", which
    // FFmpeg emits for streams with no meaningful frame rate.
    static std::optional<Rational> parse(std::string_view text);

    // The nearest rational with denominator 1,000,000 - the seam where a UI's
    // double becomes exact. Returns nullopt for non-finite values and for
    // magnitudes an int64 of microseconds cannot hold. For values *entering*
    // the engine; values already Rational must never round-trip through here.
    static std::optional<Rational> approximate(double value);

    std::int64_t numerator() const { return num_; }
    std::int64_t denominator() const { return den_; }

    bool is_zero() const { return num_ == 0; }
    bool is_negative() const { return num_ < 0; }

    // The multiplicative inverse. Precondition: not zero.
    Rational recip() const;

    // Largest integer <= this value.
    std::int64_t floor() const;
    // Smallest integer >= this value.
    std::int64_t ceil() const;

    Rational clamp_to(Rational low, Rational high) const;

    // Lossy conversion, for display and for arguments handed to the engine.
    // Do not convert back: a value that round-trips through double is no
    // longer exact, and that is how frame drift starts.
    double as_double() const;

    friend bool operator==(Rational a, Rational b) { return a.num_ == b.num_ && a.den_ == b.den_; }
    friend bool operator!=(Rational a, Rational b) { return !(a == b); }
    friend bool operator<(Rational a, Rational b);
    friend bool operator>(Rational a, Rational b) { return b < a; }
    friend bool operator<=(Rational a, Rational b) { return !(b < a); }
    friend bool operator>=(Rational a, Rational b) { return !(a < b); }

    friend Rational operator+(Rational a, Rational b);
    friend Rational operator-(Rational a, Rational b);
    friend Rational operator*(Rational a, Rational b);
    friend Rational operator/(Rational a, Rational b);
    friend Rational operator-(Rational a);

    std::string to_string() const;

private:
    // Tag for the already-reduced private constructor, so it cannot be
    // confused with the public (num, den) constructor.
    struct PrivateTag
    {
    };

    // Builds num/den in lowest terms. Precondition: den != 0.
    static Rational reduce(std::int64_t num, std::int64_t den);

    // Stores an already-reduced value without re-reducing.
    Rational(std::int64_t num, std::int64_t den, PrivateTag) : num_(num), den_(den) { }

    std::int64_t num_;
    std::int64_t den_;
};

// Frames per second, as an exact fraction. Always positive.
class FrameRate
{
public:
    static const FrameRate FILM_NTSC; // 23.976 fps (24000/1001)
    static const FrameRate FILM; // 24 fps
    static const FrameRate PAL; // 25 fps
    static const FrameRate NTSC_30; // 29.97 fps (30000/1001)
    static const FrameRate THIRTY; // 30 fps
    static const FrameRate NTSC_60; // 59.94 fps (60000/1001)
    static const FrameRate SIXTY; // 60 fps

    // Precondition: fps is positive.
    explicit FrameRate(Rational fps) : fps_(fps) { }

    static FrameRate from_int(std::uint32_t fps)
    {
        return FrameRate(Rational::from_int(static_cast<std::int64_t>(fps)));
    }

    Rational fps() const { return fps_; }

    // How long one frame lasts, in seconds.
    Rational frame_duration() const { return fps_.recip(); }

    // The timestamp at which frame `index` begins. Frame 0 begins at zero.
    Rational time_of_frame(std::int64_t index) const
    {
        return Rational::from_int(index) * frame_duration();
    }

    // The index of the frame on screen at `time`. Frames are half-open
    // intervals: a timestamp exactly on a boundary belongs to the frame that
    // starts there.
    std::int64_t frame_at(Rational time) const;

    // How many whole frames fit in `duration`.
    std::int64_t frames_in(Rational duration) const;

    friend bool operator==(const FrameRate &a, const FrameRate &b) { return a.fps_ == b.fps_; }

private:
    Rational fps_;
};

// A half-open span of time, [start, start + duration).
class TimeRange
{
public:
    // Precondition: duration is not negative.
    TimeRange(Rational start, Rational duration) : start_(start), duration_(duration) { }

    Rational start() const { return start_; }
    Rational duration() const { return duration_; }

    // The first instant after the span.
    Rational end() const { return start_ + duration_; }

    // True if `time` falls inside the span. The end is excluded.
    bool contains(Rational time) const { return time >= start_ && time < end(); }

    // True if the two spans share at least one instant.
    bool overlaps(const TimeRange &other) const
    {
        return start_ < other.end() && other.start_ < end();
    }

private:
    Rational start_;
    Rational duration_;
};

// ---------------------------------------------------------------------------
// Generational arena
// ---------------------------------------------------------------------------
//
// Editor data is a graph - clips reference media, effects reference clips -
// and it is modelled with handles into an arena rather than with pointers.
// The handle is copyable and comparable, so it goes into undo records and
// across threads without ceremony, and the generation counter turns a stale
// handle into nullopt instead of a use-after-free.

template <typename T>
class ArenaId
{
public:
    ArenaId() = default;

    std::uint32_t index() const { return index_; }
    std::uint32_t generation() const { return generation_; }

    friend bool operator==(ArenaId a, ArenaId b)
    {
        return a.index_ == b.index_ && a.generation_ == b.generation_;
    }
    friend bool operator!=(ArenaId a, ArenaId b) { return !(a == b); }

private:
    template <typename>
    friend class Arena;
    ArenaId(std::uint32_t index, std::uint32_t generation) : index_(index), generation_(generation)
    {
    }

    std::uint32_t index_ = 0;
    std::uint32_t generation_ = 0;
};

// A slab of T addressed by ArenaId, where removed slots are reused without
// letting old handles alias the new occupant.
template <typename T>
class Arena
{
public:
    ArenaId<T> insert(T value)
    {
        if (!free_.empty()) {
            const std::uint32_t index = free_.back();
            free_.pop_back();
            Slot &slot = slots_[index];
            slot.occupied = true;
            slot.value = std::move(value);
            ++len_;
            return ArenaId<T>(index, slot.generation);
        }
        const std::uint32_t index = static_cast<std::uint32_t>(slots_.size());
        slots_.push_back(Slot{ true, 0, std::move(value) });
        ++len_;
        return ArenaId<T>(index, 0);
    }

    T *get(ArenaId<T> id)
    {
        if (id.index_ >= slots_.size()) {
            return nullptr;
        }
        Slot &slot = slots_[id.index_];
        if (!slot.occupied || slot.generation != id.generation_) {
            return nullptr;
        }
        return &slot.value;
    }

    const T *get(ArenaId<T> id) const { return const_cast<Arena *>(this)->get(id); }

    // Removes the value, invalidating every handle to it. Returns nullopt if
    // the handle is already stale.
    std::optional<T> remove(ArenaId<T> id)
    {
        if (id.index_ >= slots_.size()) {
            return std::nullopt;
        }
        Slot &slot = slots_[id.index_];
        if (!slot.occupied || slot.generation != id.generation_) {
            return std::nullopt;
        }
        std::optional<T> value = std::move(slot.value);
        slot.occupied = false;
        ++slot.generation;
        free_.push_back(id.index_);
        --len_;
        return value;
    }

    std::size_t size() const { return len_; }
    bool empty() const { return len_ == 0; }

    // Iterates occupied slots in index order.
    template <typename Fn>
    void for_each(Fn &&fn) const
    {
        for (std::uint32_t i = 0; i < slots_.size(); ++i) {
            const Slot &slot = slots_[i];
            if (slot.occupied) {
                fn(ArenaId<T>(i, slot.generation), slot.value);
            }
        }
    }

    template <typename Fn>
    void for_each_mut(Fn &&fn)
    {
        for (std::uint32_t i = 0; i < slots_.size(); ++i) {
            Slot &slot = slots_[i];
            if (slot.occupied) {
                fn(ArenaId<T>(i, slot.generation), slot.value);
            }
        }
    }

private:
    struct Slot
    {
        bool occupied = false;
        std::uint32_t generation = 0;
        T value{ };
    };

    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_;
    std::size_t len_ = 0;
};

// ---------------------------------------------------------------------------
// The edit
// ---------------------------------------------------------------------------

enum class TrackKind { Video, Audio };

// Where a clip's pixels come from. A path today; when a media library lands
// this becomes a handle into it, and this is the one type that has to change.
class MediaRef
{
public:
    explicit MediaRef(std::string path) : path_(std::move(path)) { }
    const std::string &path() const { return path_; }

    friend bool operator==(const MediaRef &a, const MediaRef &b) { return a.path_ == b.path_; }

private:
    std::string path_;
};

// How a layer's colour meets what is beneath it. Source-over is the ordinary
// case; the rest are the fixed-function blends a GPU offers over premultiplied
// colour, spelled the same way on the CPU so the two paths agree.
enum class Blend { Normal, Multiply, Screen, Add, Lighten, Darken };

// The mode's name in a document: "normal", "multiply", ...
std::string_view blend_name(Blend blend);
// The mode a document names; Normal for anything unknown.
Blend blend_parse(std::string_view name);

// A clip's placement in the output frame. Deliberately resolution-independent:
// `scale` is relative to the fitted size and the offsets are fractions of the
// frame, so the same transform means the same picture on a 720p export and a
// 4K one.
struct Transform
{
    double scale = 1.0;
    double offset_x = 0.0;
    double offset_y = 0.0;
    double rotation = 0.0;
    double stretch_x = 1.0;
    double stretch_y = 1.0;

    static const Transform IDENTITY;

    bool is_identity() const
    {
        return scale == 1.0 && offset_x == 0.0 && offset_y == 0.0 && rotation == 0.0
                && stretch_x == 1.0 && stretch_y == 1.0;
    }

    friend bool operator==(const Transform &a, const Transform &b)
    {
        return a.scale == b.scale && a.offset_x == b.offset_x && a.offset_y == b.offset_y
                && a.rotation == b.rotation && a.stretch_x == b.stretch_x
                && a.stretch_y == b.stretch_y;
    }
};

// A span of one piece of media, placed on a track.
class Clip
{
public:
    Clip(MediaRef media, Rational start, Rational duration)
        : media_(std::move(media)), start_(start), duration_(duration)
    {
    }

    const MediaRef &media() const { return media_; }
    Rational source_start() const { return source_start_; }
    Rational start() const { return start_; }
    Rational duration() const { return duration_; }
    Rational speed() const { return speed_; }
    float opacity() const { return opacity_; }
    Rational video_fade_in() const { return video_fade_in_; }
    Rational video_fade_out() const { return video_fade_out_; }
    const Transform &transform() const { return transform_; }
    Blend blend() const { return blend_; }

    void set_source_start(Rational value) { source_start_ = value; }
    void set_start(Rational value) { start_ = value; }
    void set_duration(Rational value) { duration_ = value; }
    // Precondition: value is positive. This is the only definition of
    // retiming; everything downstream derives from it, so picture and sound
    // cannot disagree about what a sped-up clip means.
    void set_speed(Rational value) { speed_ = value; }
    void set_opacity(float value) { opacity_ = value; }
    void set_video_fade_in(Rational value) { video_fade_in_ = value; }
    void set_video_fade_out(Rational value) { video_fade_out_ = value; }
    void set_transform(Transform value) { transform_ = value; }
    void set_blend(Blend value) { blend_ = value; }

    // Where in the clip `time` falls, 0..=1; zero outside it.
    double fraction_at(Rational time) const;

    // The opacity ramp factor at `time`, in 0..=1. One at any instant outside
    // the fade windows, and always one for a clip with no fades. This lives on
    // the clip, next to source_time_at, because it is the same kind of fact:
    // the one definition every renderer has to agree with.
    float video_fade_factor(Rational time) const;

    // The span of timeline this clip occupies.
    TimeRange range() const { return TimeRange(start_, duration_); }

    // True if the clip is on screen at `time`.
    bool contains(Rational time) const { return range().contains(time); }

    // Converts a timeline timestamp into a timestamp within the source media,
    // or nullopt if the clip is not on screen then.
    //
    // An affine map: source_start + (time - start) * speed. This is the one
    // piece of arithmetic that every trim, ripple, slip and retime operation
    // ultimately has to agree with, so it lives in exactly one place.
    std::optional<Rational> source_time_at(Rational time) const;

    // How much of the source this clip consumes: duration * speed.
    Rational source_duration() const { return duration_ * speed_; }

private:
    MediaRef media_;
    Rational source_start_ = Rational::ZERO;
    Rational start_;
    Rational duration_;
    Rational speed_ = Rational::ONE;
    float opacity_ = 1.0f;
    Rational video_fade_in_ = Rational::ZERO;
    Rational video_fade_out_ = Rational::ZERO;
    Transform transform_ = Transform::IDENTITY;
    Blend blend_ = Blend::Normal;
};

// A horizontal lane of clips.
class Track
{
public:
    Track(std::string name, TrackKind kind) : name_(std::move(name)), kind_(kind) { }

    const std::string &name() const { return name_; }
    TrackKind kind() const { return kind_; }
    bool enabled() const { return enabled_; }
    void set_enabled(bool value) { enabled_ = value; }

    const std::vector<ArenaId<Clip>> &clips() const { return clips_; }

private:
    friend class Timeline;
    std::string name_;
    TrackKind kind_;
    bool enabled_ = true;
    // Clips in insertion order - the source of truth for the track's contents.
    // `clip_on_track_at`'s "last-added wins" tie-break reads this order.
    std::vector<ArenaId<Clip>> clips_;

    // A time-sorted cache over `clips_`, built lazily by the first query after
    // a mutation. It exists only to turn the playhead lookup from O(n) into
    // O(log n); it never changes what a query answers. Invariants (Timeline is
    // the only writer, and every add/remove that touches `clips_` marks the
    // track dirty, so the cache always reflects `clips_` when a query reads it):
    //   - `index_` holds one entry per clip, sorted by (start, then insertion
    //     order among survivors) ascending.
    //   - `index_max_end_[i]` is the greatest `end` over `index_[0..i]`.
    //   - `index_dirty_` is true exactly when `index_` is stale.
    // A clip's placement is immutable after add_clip: the only supported moves
    // are add_clip / remove_clip / remove_track, all of which mark the track.
    // (Clip::set_start configures a clip before it is placed; moving a placed
    // clip is a remove followed by an add, never an in-place set_start.)
    struct IndexEntry
    {
        Rational start;
        Rational end;
        std::size_t seq; // insertion order among the track's clips
        ArenaId<Clip> id;
    };
    mutable std::vector<IndexEntry> index_;
    mutable std::vector<Rational> index_max_end_;
    mutable bool index_dirty_ = true;
};

using TrackId = ArenaId<Track>;
using ClipId = ArenaId<Clip>;

// The edit: output format, tracks, and the clips on them.
//
// Track order is explicit and bottom-first: track_ids()[0] is composited first
// and everything after it draws on top.
class Timeline
{
public:
    Timeline(std::uint32_t width, std::uint32_t height, FrameRate frame_rate)
        : frame_rate_(frame_rate), width_(width), height_(height)
    {
    }

    // A 1920x1080, 30 fps timeline - the default for a new project.
    static Timeline hd() { return Timeline(1920, 1080, FrameRate::THIRTY); }

    FrameRate frame_rate() const { return frame_rate_; }
    std::uint32_t width() const { return width_; }
    std::uint32_t height() const { return height_; }

    // Adds a track on top of the existing ones.
    TrackId add_track(Track track);

    // Track handles, bottom-most first. Composite in this order.
    const std::vector<TrackId> &track_ids() const { return order_; }

    Track *track(TrackId id) { return tracks_.get(id); }
    const Track *track(TrackId id) const { return tracks_.get(id); }

    // Removes a track and every clip on it.
    std::optional<Track> remove_track(TrackId id);

    // Places a clip on a track. Returns nullopt if the track handle is stale.
    std::optional<ClipId> add_clip(TrackId track, Clip clip);

    Clip *clip(ClipId id) { return clips_.get(id); }
    const Clip *clip(ClipId id) const { return clips_.get(id); }

    // Removes a clip from wherever it sits.
    std::optional<Clip> remove_clip(ClipId id);

    // How many clips the timeline holds, across all tracks.
    std::size_t clip_count() const { return clips_.size(); }

    // The clip visible on `track` at `time`, if any. Clips on one track are
    // not supposed to overlap; if they do, the last-added one wins, which
    // matches what you see after a paste. Answered from the per-track
    // time-sorted index: O(log n) on a non-overlapping track, and only an
    // overlap extends the search beyond one containment test.
    std::optional<ClipId> clip_on_track_at(TrackId track, Rational time) const;

    // Where the last clip ends. Zero for an empty timeline.
    Rational duration() const;

    // How many whole output frames the timeline runs for.
    std::int64_t frame_count() const { return frame_rate_.frames_in(duration()); }

private:
    // Rebuilds `track`'s time-sorted index cache from its clip list. See
    // Track::index_ for the invariants; the cache is dropped and rebuilt on
    // the next query whenever a mutation marks the track dirty.
    void rebuild_track_index(const Track &track) const;

    FrameRate frame_rate_;
    std::uint32_t width_;
    std::uint32_t height_;
    Arena<Track> tracks_;
    Arena<Clip> clips_;
    std::vector<TrackId> order_;
};

} // namespace genesis::core

#endif // GENESIS0_CORE_CLIP_TIMELINE_H
