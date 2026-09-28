// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstddef>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace genesis::core {

// Speed that changes over a clip.
//
// A constant speed is one number. A curve is speed as a function of where in
// the clip you are: a handful of points, (x, speed) with x the fraction of the
// clip's timeline length, joined by straight lines. The source time at any
// instant is then the area under that line up to it - which is why the curve
// lives here, beside the clip's affine map: it is the same fact, and every
// renderer has to agree with it.
//
// The units are deliberately relative. A curve stored as fractions means the
// same thing after the clip is trimmed or the project re-timed, and a preset
// can be applied to any clip without knowing how long it is.
class SpeedCurve
{
public:
    // The floor a curve's speed is held at: a stop would consume no source and
    // the map would stop being invertible.
    static constexpr double MIN_SPEED = 0.0625;
    // And the ceiling, matching the constant rate's.
    static constexpr double MAX_SPEED = 16.0;

    // A curve through these points, tidied: sorted by x, clamped into the unit
    // interval and the speed range, and anchored at both ends so the line
    // covers the whole clip. Fewer than one point is nullopt.
    static std::optional<SpeedCurve> create(const std::vector<std::pair<double, double>> &points);

    // The points, as tidied.
    const std::vector<std::pair<double, double>> &points() const { return points_; }

    // Speed at x, a fraction of the clip's length.
    double speed_at(double x) const;

    // The area under the curve from the start to x: how much of the source, as
    // a fraction of the clip's timeline length, has been consumed by then. Each
    // segment is a trapezoid, so this is exact.
    double consumed(double x) const;

    // The average speed: source seconds consumed per timeline second over the
    // whole clip.
    double mean() const;

    // The clip cut into count equal timeline pieces, each
    // (x0, x1, consumed_at_x0, mean_speed) - the constant-rate approximation a
    // sound path that can only change tempo in steps needs.
    std::vector<std::tuple<double, double, double, double>> pieces(std::size_t count) const;

private:
    explicit SpeedCurve(std::vector<std::pair<double, double>> points) : points_(std::move(points))
    {
    }

    std::vector<std::pair<double, double>> points_;
};

} // namespace genesis::core
