// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "project/commands/Apply.h"
#include "project/IdMinter.h"
#include "project/Project.h"
#include "project/Undo.h"

namespace genesis::project {

// One editing session: the project, the id mint that keeps its ids unique,
// and the undo/redo stacks. The object a host holds per open project;
// everything else in the layer is reachable through it.
class Editor
{
public:
    Editor() = default;

    // Applies one command, recording the state before it for undo. A
    // command that fails leaves the project and history untouched, and a
    // tolerated no-op records nothing.
    ApplyResult apply(const Command &command);

    // `apply` as one move of a gesture. A command naming the same gesture
    // as the last recorded step folds into it, so undo lands where the
    // gesture began; `nullopt` is a step of its own.
    ApplyResult apply_within(const std::optional<std::string> &gesture, const Command &command);

    bool can_undo() const { return history_.can_undo(); }
    bool can_redo() const { return history_.can_redo(); }

    // Steps back one edit, restoring every entity the edit touched.
    void undo();
    // Steps forward again, re-applying the stored command.
    void redo();

    // Ends the gesture in progress, if any.
    void end_gesture() { history_.end_gesture(); }

    // Whether a change has occurred since the last save.
    bool dirty() const { return history_.dirty(); }
    void mark_saved() { history_.mark_saved(); }

    // The current state, read-only; all mutation goes through `apply` so
    // nothing can change without being undoable.
    Project &project() { return project_; }
    const Project &project() const { return project_; }

    // The id the mint will next produce for `prefix`, without consuming it -
    // what the next AddMedia (prefix "m") mints. Lets a host build one Batch
    // that pairs an AddMedia with an AddClip naming that media, so a single
    // undo step covers both (see IdMinter::peek).
    std::string peek_id(std::string_view prefix) const { return mint_.peek(prefix); }

    // Replaces the project, resetting the mint and the history so the editor
    // starts clean: the mint adopts every id the restored project uses, and
    // undo/redo/dirty begin empty. The Editor object itself is not recreated,
    // so a host holding its address keeps working across a load.
    void load(Project project);

    // A monotonically increasing counter bumped on every `load`. An async
    // worker captures it when a run starts and re-checks it in its finish
    // path, so a result computed against a project that has since been
    // swapped out is discarded instead of applied to the wrong one. Both the
    // bump (`load`) and the reads (`epoch`) happen on the UI thread, so no
    // atomic is needed.
    std::uint64_t epoch() const { return epoch_; }

private:
    // Applies `command` and runs the derived steps, extending `record`.
    // Shared by apply_within (which then records history) and redo (which
    // discards it, since the undo stack already holds the before-image).
    ApplyResult apply_and_derive(const Command &command, UndoRecord &record);

    // Runs `Clip::tidy` over every clip the command wrote, so the clamps
    // live in one place and a document is the same on screen as it is after
    // a save and a reopen (audit 2026-09-23, #13). Each clip it changes is
    // recorded into `record` so undo takes the tidying back with the edit.
    void tidy_touched(UndoRecord &record, const std::vector<std::string> &written_clips);

    // Makes an SDR timeline HLG when the command put its first HDR clip on
    // it - a clip of a file recorded in HLG or PQ, where before it had none -
    // in the same step, so an undo takes both back. A timeline set back to
    // SDR with HDR clips already on it stays SDR, and only a timeline the
    // command wrote, and did not make, is looked at. The timeline's
    // before-image is recorded so undo takes the promotion back.
    void follow_first_hdr(UndoRecord &record, const std::vector<std::string> &written_clips,
                          const std::vector<std::string> &written_tracks);

    Project project_;
    IdMinter mint_;
    History history_;
    std::uint64_t epoch_ = 0;
};

} // namespace genesis::project
