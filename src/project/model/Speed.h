// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Speed model: a point of a clip's speed curve and a transition on a cut.

#pragma once

#include <string>

#include "core/ClipTimeline.h"

namespace genesis::project {

using genesis::core::Rational;

// One point of a speed curve.
// On disk: camelCase.
struct SpeedPoint
{
    // Where in the clip, as a fraction of its timeline length, 0..=1.
    double at;
    // Source seconds per timeline second there.
    double speed;

    bool operator==(const SpeedPoint &) const = default;
};

// A transition on the cut into a clip.
// On disk: camelCase.
struct Transition
{
    // Which transition from the catalogue, e.g. "cross-fade".
    std::string id;
    // Seconds the transition covers.
    Rational duration = Rational::ONE;

    bool operator==(const Transition &) const = default;
};

} // namespace genesis::project
