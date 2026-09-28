// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Ported from Concat's concat-core/src/retime.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.

#include "core/SpeedCurve.h"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace genesis::core {

namespace {

double clamp(double value, double low, double high)
{
    return std::min(std::max(value, low), high);
}

} // namespace

std::optional<SpeedCurve> SpeedCurve::create(const std::vector<std::pair<double, double>> &points)
{
    std::vector<std::pair<double, double>> tidied;
    tidied.reserve(points.size());
    for (const auto &[x, speed] : points) {
        if (!std::isfinite(x) || !std::isfinite(speed))
            continue;
        tidied.emplace_back(clamp(x, 0.0, 1.0), clamp(speed, MIN_SPEED, MAX_SPEED));
    }
    if (tidied.empty())
        return std::nullopt;

    std::sort(tidied.begin(), tidied.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });

    // Anchor both ends so the line covers the whole clip.
    if (tidied.front().first > 0.0) {
        tidied.insert(tidied.begin(), { 0.0, tidied.front().second });
    }
    if (tidied.back().first < 1.0) {
        tidied.emplace_back(1.0, tidied.back().second);
    }
    return SpeedCurve(std::move(tidied));
}

double SpeedCurve::speed_at(double x) const
{
    x = clamp(x, 0.0, 1.0);
    for (std::size_t i = 0; i + 1 < points_.size(); ++i) {
        const auto [x0, v0] = points_[i];
        const auto [x1, v1] = points_[i + 1];
        if (x <= x1) {
            if (x1 <= x0)
                return v1;
            return v0 + (v1 - v0) * (x - x0) / (x1 - x0);
        }
    }
    return points_.back().second;
}

double SpeedCurve::consumed(double x) const
{
    x = clamp(x, 0.0, 1.0);
    double area = 0.0;
    for (std::size_t i = 0; i + 1 < points_.size(); ++i) {
        const auto [x0, v0] = points_[i];
        const double x1 = points_[i + 1].first;
        if (x <= x0)
            break;
        const double end = std::min(x, x1);
        const double v_end = speed_at(end);
        area += (end - x0) * (v0 + v_end) / 2.0;
        if (x <= x1)
            break;
    }
    return area;
}

double SpeedCurve::mean() const
{
    return std::max(consumed(1.0), MIN_SPEED);
}

std::vector<std::tuple<double, double, double, double>> SpeedCurve::pieces(std::size_t count) const
{
    count = std::max<std::size_t>(count, 1);
    std::vector<std::tuple<double, double, double, double>> out;
    out.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const double x0 = static_cast<double>(index) / static_cast<double>(count);
        const double x1 = static_cast<double>(index + 1) / static_cast<double>(count);
        const double from = consumed(x0);
        const double to = consumed(x1);
        out.emplace_back(x0, x1, from, std::max((to - from) / (x1 - x0), MIN_SPEED));
    }
    return out;
}

} // namespace genesis::core
