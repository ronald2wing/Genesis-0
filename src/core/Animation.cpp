// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "core/Animation.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace genesis::core {

namespace {

double clamp(double value, double low, double high)
{
    return std::min(std::max(value, low), high);
}

// x of a cubic bezier with endpoints pinned at 0 and 1, at parameter t.
double bezier_axis(double p1, double p2, double t)
{
    const double u = 1.0 - t;
    return (3.0 * u * u * t * p1) + (3.0 * u * t * t * p2) + (t * t * t);
}

double bezier_axis_slope(double p1, double p2, double t)
{
    const double u = 1.0 - t;
    return (3.0 * u * u * p1) + (6.0 * u * t * (p2 - p1)) + (3.0 * t * t * (1.0 - p2));
}

} // namespace

bool Ease::is_linear() const
{
    return std::abs(x1 - y1) < 1e-6 && std::abs(x2 - y2) < 1e-6;
}

double Ease::apply(double t) const
{
    if (is_linear()) {
        return clamp(t, 0.0, 1.0);
    }
    return bezier_y_at_x(x1, y1, x2, y2, t);
}

double bezier_y_at_x(double x1, double y1, double x2, double y2, double x)
{
    x = clamp(x, 0.0, 1.0);
    double t = x;

    for (int i = 0; i < 8; ++i) {
        const double error = bezier_axis(x1, x2, t) - x;
        if (std::abs(error) < 1e-7) {
            return bezier_axis(y1, y2, t);
        }
        const double slope = bezier_axis_slope(x1, x2, t);
        if (std::abs(slope) < 1e-9) {
            break;
        }
        t -= error / slope;
    }

    double lo = 0.0;
    double hi = 1.0;
    t = x;
    for (int i = 0; i < 32; ++i) {
        const double at = bezier_axis(x1, x2, t);
        if (std::abs(at - x) < 1e-7) {
            break;
        }
        if (at > x) {
            hi = t;
        } else {
            lo = t;
        }
        t = (lo + hi) / 2.0;
    }
    return bezier_axis(y1, y2, t);
}

KeyTrack::KeyTrack(std::vector<Key> keys) : keys_(std::move(keys))
{
    keys_.erase(std::remove_if(keys_.begin(), keys_.end(),
                               [](const Key &key) {
                                   return !std::isfinite(key.at) || !std::isfinite(key.value);
                               }),
                keys_.end());
    for (auto &key : keys_) {
        key.at = clamp(key.at, 0.0, 1.0);
    }
    std::stable_sort(keys_.begin(), keys_.end(),
                     [](const Key &a, const Key &b) { return a.at < b.at; });
}

double KeyTrack::value_at(double x, double rest) const
{
    if (keys_.empty()) {
        return rest;
    }
    if (x <= keys_.front().at) {
        return keys_.front().value;
    }
    for (std::size_t i = 0; i + 1 < keys_.size(); ++i) {
        const Key &a = keys_[i];
        const Key &b = keys_[i + 1];
        if (x <= b.at) {
            if (b.at <= a.at) {
                return b.value;
            }
            const double t = b.ease.apply((x - a.at) / (b.at - a.at));
            return a.value + ((b.value - a.value) * t);
        }
    }
    return keys_.back().value;
}

KeyTrack KeyTrack::resample(std::size_t steps) const
{
    steps = std::max<std::size_t>(steps, 1);
    const bool all_linear = std::all_of(keys_.begin(), keys_.end(),
                                        [](const Key &key) { return key.ease.is_linear(); });
    if (keys_.size() < 2 || all_linear) {
        return *this;
    }

    std::vector<Key> out;
    out.push_back(keys_.front());
    for (std::size_t i = 0; i + 1 < keys_.size(); ++i) {
        const Key &a = keys_[i];
        const Key &b = keys_[i + 1];
        if (b.ease.is_linear() || b.at <= a.at) {
            Key copy = b;
            copy.ease = Ease::linear();
            out.push_back(copy);
            continue;
        }
        for (std::size_t step = 1; step <= steps; ++step) {
            const double t = static_cast<double>(step) / static_cast<double>(steps);
            out.push_back(Key{ a.at + ((b.at - a.at) * t),
                               a.value + ((b.value - a.value) * b.ease.apply(t)), Ease::linear() });
        }
    }
    return KeyTrack(std::move(out));
}

bool Animation::is_empty() const
{
    return scale.is_empty() && offset_x.is_empty() && offset_y.is_empty() && rotation.is_empty()
            && opacity.is_empty() && volume.is_empty();
}

Transform Animation::transform_at(Transform base, double x) const
{
    Transform out = base;
    out.scale = std::max(base.scale * scale.value_at(x, 1.0), 0.001);
    out.offset_x = base.offset_x + offset_x.value_at(x, 0.0);
    out.offset_y = base.offset_y + offset_y.value_at(x, 0.0);
    out.rotation = base.rotation + rotation.value_at(x, 0.0);
    return out;
}

float Animation::opacity_at(float base, double x) const
{
    return static_cast<float>(
            clamp(static_cast<double>(base) * opacity.value_at(x, 1.0), 0.0, 1.0));
}

} // namespace genesis::core
