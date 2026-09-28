// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The studio shell's keyframes controller: the view state for the keyframes
// pane plus the thin mapping from pane intent to key commands, applied through
// the host Editor so every edit is undoable. It owns no QML - only the
// selection of the clip being keyed, the read-only projection of that clip's
// keys (for the pane to draw), a curve sampler that reuses the host's own
// interpolation so the drawn curve cannot disagree with playback, and the
// add/remove/ease/move gestures that land as `SetClipKey`/`ClearClipKey`/
// `ClearClipKeys` commands. The shell sets the clip id (it already tracks
// selection) and the playhead position; the QML pane binds to the properties
// below and calls the Q_INVOKABLEs.
//
// A key move is one undo step, not one per pointer sample: beginKeyMove opens
// a gesture and updateKeyMove coalesces each live update into it, exactly as
// EditController's move/trim gestures do. The pane never re-implements
// editing; it only reports positions and this controller turns them into
// commands, so undo/redo land where the gesture began.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

#include <string>

#include "project/command/Command.h"
#include "project/command/PropertyCommands.h"
#include "project/model/Clip.h"
#include "project/model/Keyframe.h"

namespace genesis::project {
class Editor;
struct Command;
} // namespace genesis::project

class KeyframesController : public QObject
{
    Q_OBJECT

    // The clip currently being keyed. Set by the shell, which already owns
    // selection; empty means nothing is selected and the pane is idle.
    Q_PROPERTY(QString clipId READ clipId WRITE setClipId NOTIFY clipIdChanged)
    // Where the playhead sits inside the clip, as a fraction 0..=1 of its
    // length - the same unit a key's `at` uses. The pane marks it; the shell
    // drives it.
    Q_PROPERTY(double playheadPosition READ playheadPosition WRITE setPlayheadPosition NOTIFY
                       playheadChanged)
    // The reason the last edit was refused, empty when the last edit applied.
    Q_PROPERTY(QString lastError READ lastError NOTIFY errorChanged)
    // Undo/redo/dirty, mirrored from the host Editor so the toolbar that
    // EditController also feeds stays in step with key edits.
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY historyChanged)

public:
    explicit KeyframesController(genesis::project::Editor *editor, QObject *parent = nullptr);

    QString clipId() const { return clipId_; }
    void setClipId(const QString &clipId);

    double playheadPosition() const { return playheadPosition_; }
    void setPlayheadPosition(double at);

    QString lastError() const { return lastError_; }

    bool canUndo() const;
    bool canRedo() const;
    bool dirty() const;

    // ---- Read-only projection for the pane. ----

    // The KeyProperty values the selected clip actually keys, as a list of
    // `{ "property": <name> }` maps in the panel's fixed order. Empty when
    // nothing is selected or nothing is keyed.
    Q_INVOKABLE QVariantList keyedProperties() const;

    // The selected property's keys in time order, each a
    // `{ "at": double, "value": double, "ease": <name> }` map. `ease` is
    // "linear"/"in"/"out"/"inOut", or "custom" for a hand-drawn bezier.
    Q_INVOKABLE QVariantList keysFor(const QString &property) const;

    // `count` samples of the selected property across `[from, to]`, each a
    // `{ "at": double, "value": double }` map, computed with the host's own
    // `Clip::value_at` so the drawn curve cannot disagree with what playback
    // will do. The first sample is at `from` and the last at `to`. Empty for
    // no selection, an unknown property, or `count < 2`. An unkeyed property
    // samples its constant, so the pane draws a flat line rather than a gap.
    Q_INVOKABLE QVariantList sample(const QString &property, double from, double to,
                                    int count) const;

    // ---- Edits, one undo step each (except a move, which is one step per
    // ---- drag). Each returns true when it applied and false when refused or
    // ---- a no-op; refusals land in `lastError`.

    // Adds (or replaces, within a hair) a key. `ease` is a preset name;
    // anything unrecognised reads as linear.
    Q_INVOKABLE bool addKey(const QString &property, double at, double value, const QString &ease);
    Q_INVOKABLE bool removeKey(const QString &property, double at);
    Q_INVOKABLE bool clearKeys(const QString &property);
    // Re-eases an existing key, keeping its position and value.
    Q_INVOKABLE bool setKeyEase(const QString &property, double at, const QString &ease);

    // ---- Key move gesture: one drag is one undo step. ----
    // beginKeyMove opens the gesture at the key grabbed at `at`;
    // updateKeyMove repositions it (absolute at + value, the key's own ease
    // preserved) and coalesces into the gesture; endKeyMove closes it.
    Q_INVOKABLE bool beginKeyMove(const QString &property, double at);
    Q_INVOKABLE void updateKeyMove(const QString &property, double newAt, double newValue);
    Q_INVOKABLE void endKeyMove();

    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

signals:
    // clipId changed: the pane re-reads the clip's keys.
    void clipIdChanged();
    // playheadPosition changed: the pane repositions its marker.
    void playheadChanged();
    // lastError changed: the pane shows or hides the refusal reason.
    void errorChanged();
    // canUndo / canRedo / dirty changed.
    void historyChanged();
    // The clip's keys changed (add/remove/ease/move/clear): the pane re-reads
    // keyedProperties and keysFor.
    void propertiesChanged();
    // The project changed and the shell should reload the engine/timeline.
    // Emitted once per edit, at the end of a gesture, not on every live update.
    void projectEdited();

private:
    enum class Gesture { None, KeyMove };

    // The selected clip on the active timeline, or null when none/unknown.
    const genesis::project::Clip *clip() const;

    // Records a refusal reason and emits errorChanged.
    void refuse(const QString &reason);
    // Clears a previously recorded refusal.
    void clearError();

    // Applies one discrete command (one undo step) and notifies; false when
    // refused or a tolerated no-op.
    bool applyEdit(const genesis::project::Command &command);

    // Ends any in-flight move gesture, committing it as its one undo step.
    void finishGesture();

    genesis::project::Editor *editor_;
    QString clipId_;
    double playheadPosition_ = 0.0;
    QString lastError_;

    Gesture gesture_ = Gesture::None;
    std::string gestureClipId_;
    genesis::project::KeyProperty gestureProperty_ = genesis::project::KeyProperty::Scale;
    double gestureOriginalAt_ = 0.0;
    genesis::project::KeyEase gestureEase_ = genesis::project::KeyEase::linear();
    std::string gestureName_;
    bool gestureApplied_ = false;
};
