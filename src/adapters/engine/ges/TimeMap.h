// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The exact rational <-> GES nanosecond seam. `as_double()` is deliberately
// lossy elsewhere; here it is the documented engine-argument edge, and the
// value is never converted back.
//
// A non-finite or overflowing value must never reach GES: `llround` of a NaN
// or an out-of-range double is undefined and lands on G_MININT64, which GES
// then hands to the NLE object's `duration` property (a gint64 bounded below
// by 0) and trips a GObject range warning. Clamp to the valid GstClockTime
// range instead, so one degenerate duration cannot poison the build.

#pragma once

#include <cmath>
#include <cstdint>

#include <gst/gst.h>

#include "core/ClipTimeline.h"

namespace genesis::adapters::engine::ges {

inline GstClockTime to_ns(core::Rational time)
{
    const double ns = time.as_double() * GST_SECOND;
    if (!std::isfinite(ns) || ns <= 0.0) {
        return 0;
    }
    if (ns >= static_cast<double>(G_MAXINT64)) {
        return static_cast<GstClockTime>(G_MAXINT64);
    }
    return static_cast<GstClockTime>(std::llround(ns));
}

// Nanoseconds are already an exact integer, so the rational is exact and only
// reduces to lowest terms.
inline core::Rational from_ns(GstClockTime ns)
{
    return core::Rational(static_cast<std::int64_t>(ns), static_cast<std::int64_t>(GST_SECOND));
}

} // namespace genesis::adapters::engine::ges
