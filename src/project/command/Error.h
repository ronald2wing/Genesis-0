// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Command errors and outcomes: what a command produced and why one was refused.

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace genesis::project {

// What a command produced, beyond the new state: the ids it minted, so the
// UI can select what it just created, and whether anything changed at all.
// On disk: camelCase.
struct Outcome
{
    // The id of what the command created - a clip, track, timeline, or
    // media item - or, for a batch, the last id minted inside it. Absent
    // when nothing was created, including tolerated no-ops.
    // On disk: skipped when absent.
    std::optional<std::string> created_id;
    // Whether the command actually changed the project. A tolerated no-op -
    // a missing id, a field set to the value it already had - succeeds but
    // reports false, which is how the editor knows not to record an undo
    // snapshot for it. For a batch: whether any member changed anything.
    // On disk: default.
    bool applied = false;

    bool operator==(const Outcome &) const = default;
};

// Why a command was refused. Every variant renders as the exact
// user-facing sentence the UI shows - byte for byte the strings the window
// treats as the contract; the enum only gives those sentences names a host
// can match on.
enum class CommandErrorKind {
    // Command::FillSlot named a media id no longer in the bin.
    SlotGone,
    // Command::FillSlot targeted ordinary media rather than a slot.
    NotASlot,
    // A clip-placing command named media no longer in the bin.
    MediaGone,
    // A clip-placing command named a track no longer on the timeline.
    TrackGone,
    // A first-free-track placement found a timeline with no tracks at all.
    NoTracks,
    // Command::MergeClips was refused, for whichever why_not_merge reason
    // applied.
    CannotMerge,
    // Command::RemoveTrack would have deleted the last track.
    LastTrack,
    // Command::RemoveTimeline would have deleted the last timeline.
    LastTimeline,
    // A number in the command is NaN or infinite. No edit means one, and
    // one stored would poison every duration and key that touched it.
    NotANumber,
    // Command::SetTimelineVideo named a frame or rate outside the editor's
    // output bounds. `reason` names the field and its range.
    VideoOutOfRange,
};

// The refusal itself: which variant, and the data `CannotMerge` carries.
// `message()` is the `Display` output - the sentence the UI shows verbatim.
struct CommandError
{
    // Which refusal this is.
    CommandErrorKind kind = CommandErrorKind::SlotGone;
    // The why_not_merge sentence, verbatim. Empty for every other variant.
    std::string reason;

    bool operator==(const CommandError &) const = default;

    // The exact user-facing sentence the UI shows, byte for byte - the
    // strings the window treats as the contract.
    std::string_view message() const
    {
        switch (kind) {
        case CommandErrorKind::SlotGone:
            return "That template slot no longer exists.";
        case CommandErrorKind::NotASlot:
            return "That media is not a template slot.";
        case CommandErrorKind::MediaGone:
            return "That media is no longer in the bin.";
        case CommandErrorKind::TrackGone:
            return "That track no longer exists.";
        case CommandErrorKind::NoTracks:
            return "There are no tracks.";
        case CommandErrorKind::CannotMerge:
            return reason;
        case CommandErrorKind::LastTrack:
            return "A timeline needs at least one track.";
        case CommandErrorKind::LastTimeline:
            return "A project needs at least one timeline.";
        case CommandErrorKind::NotANumber:
            return "A number in that edit is not finite.";
        case CommandErrorKind::VideoOutOfRange:
            return reason;
        }
        return "That template slot no longer exists.";
    }
};

} // namespace genesis::project
