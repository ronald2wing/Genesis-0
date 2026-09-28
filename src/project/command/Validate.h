// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Command validity checks: which commands are view state, and whether any is non-finite.

#pragma once

#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "project/command/Command.h"
#include "project/command/Error.h"
#include "project/model/Cutout.h"
#include "project/model/Effects.h"
#include "project/model/Keyframe.h"
#include "project/model/Speed.h"
#include "project/model/Text.h"
#include "project/model/VideoSettings.h"

namespace genesis::project {

// True for a command that changes what the person is looking at, not
// the edit: which timeline tab is showing and the order of the tabs.
// Saved with the document, never recorded in undo history.
inline bool is_view_state(const Command &command)
{
    return std::holds_alternative<SelectTimeline>(command.value)
            || std::holds_alternative<MoveTimeline>(command.value);
}

// True when any number in the command is NaN or infinite. JSON cannot
// spell one, but a caller in C++ can, and `NaN.clamp()` is NaN: one
// stored would poison every duration and key it touched, and the
// document reader would only repair it on the next load.
inline bool has_non_finite(const Command &command)
{
    const auto bad_optional = [](const std::optional<double> &value) {
        return value.has_value() && !std::isfinite(*value);
    };
    const auto bad_stroke = [](const Stroke &stroke) {
        if (!std::isfinite(stroke.size)) {
            return true;
        }
        for (const std::array<double, 2> &point : stroke.points) {
            if (!std::isfinite(point[0]) || !std::isfinite(point[1])) {
                return true;
            }
        }
        return false;
    };
    const auto bad_chain = [](const std::vector<AppliedFilter> &chain) {
        for (const AppliedFilter &entry : chain) {
            for (const auto &param : entry.params) {
                if (!std::isfinite(param.second)) {
                    return true;
                }
            }
            for (const auto &run : entry.keys) {
                for (const ParamKey &key : run.second) {
                    if (!std::isfinite(key.at) || !std::isfinite(key.value)
                        || !std::isfinite(key.ease.values[0])) {
                        return true;
                    }
                }
            }
        }
        return false;
    };
    const auto bad_style = [](const TextStyle &style) {
        return !std::isfinite(style.font_size) || !std::isfinite(style.font_weight)
                || !std::isfinite(style.opacity) || !std::isfinite(style.stroke_width)
                || !std::isfinite(style.line_height) || !std::isfinite(style.tracking)
                || !std::isfinite(style.max_width) || !std::isfinite(style.max_height);
    };

    return std::visit(
            [&](const auto &alternative) -> bool {
                using T = std::decay_t<decltype(alternative)>;
                if constexpr (std::is_same_v<T, Batch>) {
                    for (const Command &member : alternative.commands) {
                        if (has_non_finite(member)) {
                            return true;
                        }
                    }
                    return false;
                } else if constexpr (std::is_same_v<T, AddMedia> || std::is_same_v<T, FillSlot>
                                     || std::is_same_v<T, ReplaceClipMedia>) {
                    return bad_optional(alternative.item.frame_rate);
                } else if constexpr (std::is_same_v<T, AddTextClip>) {
                    return bad_optional(alternative.offset_y)
                            || (alternative.style && bad_style(*alternative.style));
                } else if constexpr (std::is_same_v<T, SetClipSpeedCurve>) {
                    if (!alternative.curve) {
                        return false;
                    }
                    for (const SpeedPoint &point : *alternative.curve) {
                        if (!std::isfinite(point.at) || !std::isfinite(point.speed)) {
                            return true;
                        }
                    }
                    return false;
                } else if constexpr (std::is_same_v<T, SetClipKey>
                                     || std::is_same_v<T, SetEffectKey>) {
                    return !std::isfinite(alternative.at) || !std::isfinite(alternative.value)
                            || !std::isfinite(alternative.ease.values[0]);
                } else if constexpr (std::is_same_v<T, ClearClipKey>
                                     || std::is_same_v<T, ClearEffectKey>) {
                    return !std::isfinite(alternative.at);
                } else if constexpr (std::is_same_v<T, SetClipCutout>) {
                    if (!alternative.cutout) {
                        return false;
                    }
                    const Cutout &cutout = *alternative.cutout;
                    if (!std::isfinite(cutout.feather)) {
                        return true;
                    }
                    for (const Stroke &stroke : cutout.strokes) {
                        if (bad_stroke(stroke)) {
                            return true;
                        }
                    }
                    return false;
                } else if constexpr (std::is_same_v<T, AddCutoutStroke>) {
                    return bad_stroke(alternative.stroke);
                } else if constexpr (std::is_same_v<T, UpdateClip>) {
                    const ClipPatch &patch = alternative.patch;
                    if (bad_optional(patch.volume) || bad_optional(patch.opacity)) {
                        return true;
                    }
                    if (patch.crop && *patch.crop) {
                        const Crop &crop = **patch.crop;
                        if (!std::isfinite(crop.left) || !std::isfinite(crop.top)
                            || !std::isfinite(crop.right) || !std::isfinite(crop.bottom)) {
                            return true;
                        }
                    }
                    if ((patch.filters && bad_chain(*patch.filters))
                        || (patch.video_effects && bad_chain(*patch.video_effects))) {
                        return true;
                    }
                    if (patch.text && *patch.text && bad_style(**patch.text)) {
                        return true;
                    }
                    return false;
                } else if constexpr (std::is_same_v<T, SetClipSpeed>) {
                    return !std::isfinite(alternative.speed);
                } else if constexpr (std::is_same_v<T, SetClipPan>) {
                    return !std::isfinite(alternative.pan);
                } else if constexpr (std::is_same_v<T, SetClipTransform>) {
                    return bad_optional(alternative.scale) || bad_optional(alternative.offset_x)
                            || bad_optional(alternative.offset_y)
                            || bad_optional(alternative.rotation)
                            || bad_optional(alternative.stretch_x)
                            || bad_optional(alternative.stretch_y);
                } else {
                    return false;
                }
            },
            command.value);
}

// The refusal for a video setting the editor cannot output, or nullopt when
// every field is one it can: sides 16..8192 px, rate 1..240 fps. The
// sentence names the field, so the UI can say exactly which one broke.
inline std::optional<CommandError> video_range_error(const VideoSettings &video)
{
    if (!side_in_range(video.width)) {
        return CommandError{ CommandErrorKind::VideoOutOfRange,
                             "Frame width must be between 16 and 8192 pixels." };
    }
    if (!side_in_range(video.height)) {
        return CommandError{ CommandErrorKind::VideoOutOfRange,
                             "Frame height must be between 16 and 8192 pixels." };
    }
    if (!video.rate_in_range()) {
        return CommandError{ CommandErrorKind::VideoOutOfRange,
                             "Frame rate must be between 1 and 240 fps." };
    }
    return std::nullopt;
}

} // namespace genesis::project
