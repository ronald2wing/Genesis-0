// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Ported from Concat's concat-project/src/commands/mod.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.
//
// The closed command set: the Command variant, its recursive Batch, and assign().

#pragma once

#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "project/command/AudioCommands.h"
#include "project/command/ClipCommands.h"
#include "project/command/MediaCommands.h"
#include "project/command/PropertyCommands.h"
#include "project/command/TimelineCommands.h"
#include "project/command/TrackCommands.h"

namespace genesis::project {

// Forward-declared for Batch's recursive commands vector; defined
// below after all variant structs.
struct Command;

// Several commands as one atomic edit: applied to a staged copy and
// committed only if every one succeeds, then recorded as a single undo
// step. The outcome carries the last id minted inside.
struct Batch
{
    // The commands, applied in order. Nesting is legal.
    std::vector<Command> commands;

    bool operator==(const Batch &) const = default;
};

// The closed command set. A struct wrapping the variant (not a bare alias)
// because Batch recurses into Command: a std::vector<Command> member is legal
// with an incomplete Command, but no bare alias can name itself.
struct Command
{
    std::variant<AddMedia, RemoveMedia, SetMediaPlaceholder, FillSlot, Batch, AddClip,
                 AddClipAtFirstFree, AddTextClip, AddLayerClip, SetClipSpeedCurve, SetClipKey,
                 ClearClipKey, ClearClipKeys, SetEffectKey, ClearEffectKey, ClearEffectKeys,
                 SetClipCutout, AddCutoutStroke, MoveClips, TrimClip, SplitClips, ReplaceClipMedia,
                 FreezeFrame, MergeClips, RemoveClips, UpdateClip, SetClipSpeed, SetClipReverse,
                 SetClipPan, SetClipTransform, DetachAudio, ReattachAudio, AddTrack, RemoveTrack,
                 SetTrackFlag, AddTimeline, RemoveTimeline, SetTimelineVideo, RenameTimeline,
                 SelectTimeline, MoveTimeline, AddFont, RemoveFont, UpdateMediaPath,
                 SetMediaColorRange>
            value;

    bool operator==(const Command &) const = default;

    // Builds from any alternative, so `Command c = AddClip{};` reads like
    // the Rust enum it ports. Constrained to the variant's own convertible
    // alternatives; the implicit copy and move constructors keep precedence.
    template <typename T>
        requires std::is_constructible_v<decltype(value), T &&>
    Command(T &&alternative) : value(std::forward<T>(alternative))
    {
    }
};

// Assigns `value` into `slot`, reporting whether that changed anything.
// This is how command arms notice a field being set to the value it already
// holds - which must count as "nothing happened" (`Outcome::applied`
// false), or the undo history would record phantom edits.
template <typename T>
bool assign(T &slot, T value)
{
    if (slot == value) {
        return false;
    }
    slot = std::move(value);
    return true;
}

} // namespace genesis::project
