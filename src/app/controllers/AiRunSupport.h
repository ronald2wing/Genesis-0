// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Helpers shared by the AI run controllers (AiController, CutoutController,
// EnhanceController, TtsController). Both were carried verbatim by several of
// them before being gathered here.

#pragma once

#include <cstdint>

#include <QString>

#include "project/model/Media.h"

namespace genesis::app {

// A byte count rendered the way a person reads it: one decimal place, the
// largest unit that fits. What a download pane shows next to its progress bar.
inline QString human_size(std::uintmax_t bytes)
{
    const double value = static_cast<double>(bytes);
    if (bytes >= 1024 * 1024 * 1024) {
        return QStringLiteral("%1 GB").arg(value / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1);
    }
    if (bytes >= 1024 * 1024) {
        return QStringLiteral("%1 MB").arg(value / (1024.0 * 1024.0), 0, 'f', 1);
    }
    if (bytes >= 1024) {
        return QStringLiteral("%1 KB").arg(value / 1024.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 B").arg(bytes);
}

// The frame rate a run should assume for a source: the media's own fraction
// when it carries a usable one, else a rational approximation of the float,
// else 30/1. The model providers need a rate before they can name a frame.
inline void fps_from_media(const genesis::project::MediaItem &media, std::uint32_t &num,
                           std::uint32_t &den)
{
    num = 30;
    den = 1;
    const auto from_rational = [&](const genesis::core::Rational &rate) {
        if (rate.numerator() > 0 && rate.denominator() > 0) {
            num = static_cast<std::uint32_t>(rate.numerator());
            den = static_cast<std::uint32_t>(rate.denominator());
            return true;
        }
        return false;
    };
    if (media.frame_rate_fraction) {
        const auto parsed = genesis::core::Rational::parse(*media.frame_rate_fraction);
        if (parsed && from_rational(*parsed)) {
            return;
        }
    }
    if (media.frame_rate && *media.frame_rate > 0.0) {
        const auto approx = genesis::core::Rational::approximate(*media.frame_rate);
        if (approx && from_rational(*approx)) {
            return;
        }
    }
}

} // namespace genesis::app
