// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Re-derived from Concat's concat-core (time.rs, arena.rs, timeline.rs),
// GPL-3.0-or-later, Copyright (C) 2026 Jareer and Concat contributors.
// This is a C++23 re-derivation, not a transliteration; see
// docs/rewrite-plan.md section 1.

#include "core/ClipTimeline.h"

#include <algorithm>
#include <boost/multiprecision/cpp_int.hpp>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

namespace genesis::core {

// ---------------------------------------------------------------------------
// Rational
// ---------------------------------------------------------------------------

const Rational Rational::ZERO{ 0, 1 };
const Rational Rational::ONE{ 1, 1 };

namespace {

// Exact integer arithmetic wide enough for any product or sum this class
// forms: two int64 cross products (<= 128 bits) and their sum (<= 129 bits).
// Fixed-size, so it never allocates and never traps on a large timeline.
using WideInt = boost::multiprecision::number<boost::multiprecision::cpp_int_backend<
        256, 256, boost::multiprecision::signed_magnitude, boost::multiprecision::unchecked>>;

// Greatest common divisor of two integers, ignoring sign.
WideInt gcd(WideInt a, WideInt b)
{
    if (a < 0)
        a = -a;
    if (b < 0)
        b = -b;
    while (b != 0) {
        const WideInt t = a % b;
        a = b;
        b = t;
    }
    return a;
}

// Exact 64x64 multiply, widened to WideInt. A cross product that used to
// overflow 64 bits is now just a value, not a trap.
WideInt wide_mul(std::int64_t a, std::int64_t b)
{
    return WideInt(a) * WideInt(b);
}

// Narrows a WideInt to int64. The only remaining failure path is a reduced
// value that genuinely does not fit int64, i.e. a timeline that cannot be
// represented. Intermediate products no longer overflow; only an
// unrepresentable final value stops the program.
std::int64_t narrow(WideInt value)
{
    const WideInt min = std::numeric_limits<std::int64_t>::min();
    const WideInt max = std::numeric_limits<std::int64_t>::max();
    if (value < min || value > max) {
        std::abort();
    }
    return static_cast<std::int64_t>(value);
}

// Reduces num/den to lowest terms with a positive denominator and narrows both
// halves to int64.
std::pair<std::int64_t, std::int64_t> reduce_narrow(WideInt num, WideInt den)
{
    if (den < 0) {
        num = -num;
        den = -den;
    }
    const WideInt divisor = gcd(num, den);
    return { narrow(num / divisor), narrow(den / divisor) };
}

// Compares a/b against c/d without overflow, where b and d are positive.
bool ratio_less(std::int64_t a, std::int64_t b, std::int64_t c, std::int64_t d)
{
    return wide_mul(a, d) < wide_mul(c, b);
}

} // namespace

Rational Rational::reduce(std::int64_t num, std::int64_t den)
{
    assert(den != 0 && "Rational denominator must not be zero");
    const auto [n, d] = reduce_narrow(WideInt(num), WideInt(den));
    return Rational(n, d, Rational::PrivateTag{ });
}

Rational::Rational(std::int64_t num, std::int64_t den)
{
    *this = reduce(num, den);
}

std::optional<Rational> Rational::parse(std::string_view text)
{
    // Trim ASCII whitespace at both ends.
    const auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    while (!text.empty() && is_space(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && is_space(text.back()))
        text.remove_suffix(1);
    if (text.empty())
        return std::nullopt;

    const std::size_t slash = text.find('/');
    if (slash != std::string_view::npos) {
        const std::string_view num_text = text.substr(0, slash);
        const std::string_view den_text = text.substr(slash + 1);
        std::int64_t num = 0;
        std::int64_t den = 0;
        try {
            std::size_t pos = 0;
            num = std::stoll(std::string(num_text), &pos);
            if (pos != num_text.size())
                return std::nullopt;
            pos = 0;
            den = std::stoll(std::string(den_text), &pos);
            if (pos != den_text.size())
                return std::nullopt;
        } catch (...) {
            return std::nullopt;
        }
        if (den == 0)
            return std::nullopt;
        return Rational(num, den);
    }

    const std::size_t dot = text.find('.');
    if (dot != std::string_view::npos) {
        const std::string_view whole_text = text.substr(0, dot);
        const std::string_view frac_text = text.substr(dot + 1);
        if (frac_text.empty())
            return std::nullopt;
        for (const char c : frac_text) {
            if (c < '0' || c > '9')
                return std::nullopt;
        }
        std::int64_t whole = 0;
        try {
            std::size_t pos = 0;
            whole = std::stoll(std::string(whole_text), &pos);
            if (pos != whole_text.size())
                return std::nullopt;
        } catch (...) {
            return std::nullopt;
        }
        const int sign = whole_text.starts_with('-') ? -1 : 1;
        std::int64_t den = 1;
        for (std::size_t i = 0; i < frac_text.size(); ++i) {
            if (den > std::numeric_limits<std::int64_t>::max() / 10) {
                return std::nullopt;
            }
            den *= 10;
        }
        std::int64_t frac = 0;
        try {
            std::size_t pos = 0;
            frac = std::stoll(std::string(frac_text), &pos);
            if (pos != frac_text.size())
                return std::nullopt;
        } catch (...) {
            return std::nullopt;
        }
        if (whole > 0 && whole > (std::numeric_limits<std::int64_t>::max() - frac) / den) {
            return std::nullopt;
        }
        return Rational(whole * den + sign * frac, den);
    }

    try {
        std::size_t pos = 0;
        const std::int64_t value = std::stoll(std::string(text), &pos);
        if (pos != text.size())
            return std::nullopt;
        return Rational::from_int(value);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<Rational> Rational::approximate(double value)
{
    if (!std::isfinite(value))
        return std::nullopt;
    const double scaled = std::round(value * 1'000'000.0);
    if (std::abs(scaled) >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    return Rational(static_cast<std::int64_t>(scaled), 1'000'000);
}

Rational Rational::recip() const
{
    assert(!is_zero() && "cannot take the reciprocal of zero");
    return reduce(den_, num_);
}

std::int64_t Rational::floor() const
{
    // Euclidean division rounds toward negative infinity for a positive
    // denominator, which is what floor means.
    std::int64_t q = num_ / den_;
    const std::int64_t r = num_ % den_;
    if (r != 0 && num_ < 0)
        --q;
    return q;
}

std::int64_t Rational::ceil() const
{
    std::int64_t q = num_ / den_;
    const std::int64_t r = num_ % den_;
    if (r != 0 && num_ > 0)
        ++q;
    return q;
}

Rational Rational::clamp_to(Rational low, Rational high) const
{
    if (*this < low)
        return low;
    if (*this > high)
        return high;
    return *this;
}

double Rational::as_double() const
{
    return static_cast<double>(num_) / static_cast<double>(den_);
}

std::string Rational::to_string() const
{
    if (den_ == 1)
        return std::to_string(num_);
    return std::to_string(num_) + "/" + std::to_string(den_);
}

bool operator<(Rational a, Rational b)
{
    // Denominators are always positive, so cross-multiplying preserves order.
    return ratio_less(a.num_, a.den_, b.num_, b.den_);
}

Rational operator+(Rational a, Rational b)
{
    // a/b + c/d = (ad + cb) / bd. The cross products are widened so a large
    // timeline cannot overflow; only a reduced result that still does not fit
    // int64 fails.
    const WideInt ad = wide_mul(a.num_, b.den_);
    const WideInt cb = wide_mul(b.num_, a.den_);
    const WideInt bd = wide_mul(a.den_, b.den_);
    const auto [n, d] = reduce_narrow(ad + cb, bd);
    return Rational(n, d, Rational::PrivateTag{ });
}

Rational operator-(Rational a, Rational b)
{
    return a + (-b);
}

Rational operator*(Rational a, Rational b)
{
    const WideInt num = wide_mul(a.num_, b.num_);
    const WideInt den = wide_mul(a.den_, b.den_);
    const auto [n, d] = reduce_narrow(num, den);
    return Rational(n, d, Rational::PrivateTag{ });
}

Rational operator/(Rational a, Rational b)
{
    assert(!b.is_zero() && "cannot divide a Rational by zero");
    const WideInt num = wide_mul(a.num_, b.den_);
    const WideInt den = wide_mul(a.den_, b.num_);
    const auto [n, d] = reduce_narrow(num, den);
    return Rational(n, d, Rational::PrivateTag{ });
}

Rational operator-(Rational a)
{
    return Rational(-a.num_, a.den_);
}

// ---------------------------------------------------------------------------
// FrameRate
// ---------------------------------------------------------------------------

const FrameRate FrameRate::FILM_NTSC{ Rational(24000, 1001) };
const FrameRate FrameRate::FILM{ Rational(24, 1) };
const FrameRate FrameRate::PAL{ Rational(25, 1) };
const FrameRate FrameRate::NTSC_30{ Rational(30000, 1001) };
const FrameRate FrameRate::THIRTY{ Rational(30, 1) };
const FrameRate FrameRate::NTSC_60{ Rational(60000, 1001) };
const FrameRate FrameRate::SIXTY{ Rational(60, 1) };

std::int64_t FrameRate::frame_at(Rational time) const
{
    return (time * fps_).floor();
}

std::int64_t FrameRate::frames_in(Rational duration) const
{
    return (duration * fps_).floor();
}

// ---------------------------------------------------------------------------
// Blend
// ---------------------------------------------------------------------------

std::string_view blend_name(Blend blend)
{
    switch (blend) {
    case Blend::Normal:
        return "normal";
    case Blend::Multiply:
        return "multiply";
    case Blend::Screen:
        return "screen";
    case Blend::Add:
        return "add";
    case Blend::Lighten:
        return "lighten";
    case Blend::Darken:
        return "darken";
    }
    return "normal";
}

Blend blend_parse(std::string_view name)
{
    // Trim ASCII whitespace.
    while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) {
        name.remove_prefix(1);
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
        name.remove_suffix(1);
    }
    if (name == "multiply")
        return Blend::Multiply;
    if (name == "screen")
        return Blend::Screen;
    if (name == "add")
        return Blend::Add;
    if (name == "lighten")
        return Blend::Lighten;
    if (name == "darken")
        return Blend::Darken;
    return Blend::Normal;
}

// ---------------------------------------------------------------------------
// Transform
// ---------------------------------------------------------------------------

const Transform Transform::IDENTITY{ };

// ---------------------------------------------------------------------------
// Clip
// ---------------------------------------------------------------------------

double Clip::fraction_at(Rational time) const
{
    if (duration_.is_zero())
        return 0.0;
    const double value = ((time - start_) / duration_).as_double();
    return std::clamp(value, 0.0, 1.0);
}

float Clip::video_fade_factor(Rational time) const
{
    float factor = 1.0f;
    const Rational local = time - start_;

    if (!video_fade_in_.is_zero() && local < video_fade_in_) {
        const double value = (local / video_fade_in_).as_double();
        factor *= static_cast<float>(std::clamp(value, 0.0, 1.0));
    }
    const Rational remaining = duration_ - local;
    if (!video_fade_out_.is_zero() && remaining < video_fade_out_) {
        const double value = (remaining / video_fade_out_).as_double();
        factor *= static_cast<float>(std::clamp(value, 0.0, 1.0));
    }
    return factor;
}

std::optional<Rational> Clip::source_time_at(Rational time) const
{
    if (!contains(time))
        return std::nullopt;
    const Rational forward = (time - start_) * speed_;
    return source_start_ + forward;
}

// ---------------------------------------------------------------------------
// Timeline
// ---------------------------------------------------------------------------

TrackId Timeline::add_track(Track track)
{
    const TrackId id = tracks_.insert(std::move(track));
    order_.push_back(id);
    return id;
}

std::optional<Track> Timeline::remove_track(TrackId id)
{
    std::optional<Track> track = tracks_.remove(id);
    if (!track)
        return std::nullopt;
    for (const ClipId clip : track->clips()) {
        clips_.remove(clip);
    }
    order_.erase(std::remove(order_.begin(), order_.end(), id), order_.end());
    return track;
}

std::optional<ClipId> Timeline::add_clip(TrackId track, Clip clip)
{
    // Insert into the clip arena only once we know the track is real,
    // otherwise a stale handle leaks an orphaned clip.
    Track *target = tracks_.get(track);
    if (target == nullptr)
        return std::nullopt;
    const ClipId id = clips_.insert(std::move(clip));
    target->clips_.push_back(id);
    target->index_dirty_ = true;
    return id;
}

std::optional<Clip> Timeline::remove_clip(ClipId id)
{
    std::optional<Clip> clip = clips_.remove(id);
    if (!clip)
        return std::nullopt;
    tracks_.for_each_mut([id](TrackId, Track &track) {
        auto &clips = track.clips_;
        const std::size_t before = clips.size();
        clips.erase(std::remove(clips.begin(), clips.end(), id), clips.end());
        if (clips.size() != before) {
            track.index_dirty_ = true;
        }
    });
    return clip;
}

void Timeline::rebuild_track_index(const Track &track) const
{
    track.index_.clear();
    track.index_.reserve(track.clips_.size());
    for (std::size_t seq = 0; seq < track.clips_.size(); ++seq) {
        const Clip *clip = clips_.get(track.clips_[seq]);
        if (clip == nullptr) {
            // A clip whose arena slot vanished between mutations; remove_clip
            // would already have unlinked it, so this is purely defensive.
            continue;
        }
        track.index_.push_back(
                Track::IndexEntry{ clip->start(), clip->range().end(), seq, track.clips_[seq] });
    }
    std::sort(track.index_.begin(), track.index_.end(),
              [](const Track::IndexEntry &a, const Track::IndexEntry &b) {
                  if (a.start != b.start) {
                      return a.start < b.start;
                  }
                  return a.seq < b.seq;
              });
    track.index_max_end_.clear();
    track.index_max_end_.reserve(track.index_.size());
    for (std::size_t i = 0; i < track.index_.size(); ++i) {
        const Rational end = track.index_[i].end;
        if (i == 0) {
            track.index_max_end_.push_back(end);
        } else {
            const Rational &prev = track.index_max_end_.back();
            track.index_max_end_.push_back(end > prev ? end : prev);
        }
    }
    track.index_dirty_ = false;
}

std::optional<ClipId> Timeline::clip_on_track_at(TrackId track, Rational time) const
{
    const Track *lane = tracks_.get(track);
    if (lane == nullptr)
        return std::nullopt;
    if (lane->index_dirty_)
        rebuild_track_index(*lane);

    const std::vector<Track::IndexEntry> &index = lane->index_;
    // First entry whose start is after `time`: everything before it starts at
    // or before `time`, so it is the only set that can contain `time`.
    const auto first_after = std::upper_bound(
            index.begin(), index.end(), time,
            [](Rational t, const Track::IndexEntry &entry) { return t < entry.start; });
    if (first_after == index.begin()) {
        return std::nullopt; // no clip starts at or before `time`
    }
    const std::size_t hi = static_cast<std::size_t>(first_after - index.begin());
    const std::vector<Rational> &max_end = lane->index_max_end_;
    if (max_end[hi - 1] <= time) {
        return std::nullopt; // no candidate reaches `time`
    }
    // Last-added wins among the clips that contain `time`. The prefix max
    // bounds the walk: once it drops to <= time, no earlier clip contains
    // `time` either. On a non-overlapping track this is a single containment
    // test; only a genuine overlap (a paste) scans more than one entry.
    ClipId winner{ };
    std::size_t winner_seq = 0;
    bool found = false;
    for (std::size_t i = hi; i-- > 0;) {
        if (max_end[i] <= time) {
            break;
        }
        const Track::IndexEntry &entry = index[i];
        if (entry.end > time && (!found || entry.seq > winner_seq)) {
            winner = entry.id;
            winner_seq = entry.seq;
            found = true;
        }
    }
    return found ? std::optional<ClipId>(winner) : std::nullopt;
}

Rational Timeline::duration() const
{
    Rational longest = Rational::ZERO;
    clips_.for_each([&longest](ClipId, const Clip &clip) {
        const Rational end = clip.range().end();
        if (end > longest)
            longest = end;
    });
    return longest;
}

} // namespace genesis::core
