// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The studio shell's edit controller: the thin, testable layer that turns UI
// intent (a clip dragged, an edge pulled, a clip deleted, an undo request)
// into host commands and applies them through the project Editor. It owns no
// media, no engine and no QML - only the mapping from gesture to command and
// the undo/redo bridge - so its logic can be exercised from a standalone test
// without a GUI. The shell refreshes its timeline projection and reloads the
// engine when `projectEdited` fires; the controller itself never touches
// either.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

#include <optional>
#include <string>

namespace genesis::project {
class Editor;
struct Command;
} // namespace genesis::project

class EditController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY historyChanged)
    Q_PROPERTY(QString selectedClipId READ selectedClipId NOTIFY selectionChanged)
    // Timeline tool toggles, UI state the controller consults when it builds
    // commands - never recorded in history, never an edit on their own.
    Q_PROPERTY(bool snapEnabled READ snapEnabled WRITE setSnapEnabled NOTIFY toolsChanged)
    Q_PROPERTY(bool rippleEnabled READ rippleEnabled WRITE setRippleEnabled NOTIFY toolsChanged)

public:
    explicit EditController(genesis::project::Editor *editor, QObject *parent = nullptr);

    bool canUndo() const;
    bool canRedo() const;
    bool dirty() const;
    QString selectedClipId() const;

    bool snapEnabled() const { return snapEnabled_; }
    void setSnapEnabled(bool value);
    bool rippleEnabled() const { return rippleEnabled_; }
    void setRippleEnabled(bool value);

    // ---- Move gesture: one drag is one undo step. ----
    // beginMove opens the gesture; updateMove repositions the clip (absolute
    // start, seconds) and coalesces into it; endMove closes it. `playhead` is
    // the session position and `secsPerPx` the strip's current scale, both
    // used only when snapping is on to pull a dragged edge to a nearby target;
    // they default to "no pixel scale", which leaves the frame grid alone.
    Q_INVOKABLE void beginMove(const QString &clipId);
    Q_INVOKABLE void updateMove(const QString &clipId, double newStart, double playhead = 0.0,
                                double secsPerPx = 0.0);
    Q_INVOKABLE void endMove();

    // ---- Trim gesture: one drag is one undo step. ----
    // `edge` is 0 for the head (TrimEdge::Start) and 1 for the tail
    // (TrimEdge::End). `delta` is the cumulative seconds the edge has been
    // dragged from where the gesture began (positive shortens the head or
    // lengthens the tail). The controller folds every update into one step so
    // redo replays the whole drag, not its last increment.
    Q_INVOKABLE void beginTrim(const QString &clipId, int edge);
    Q_INVOKABLE void updateTrim(const QString &clipId, int edge, double delta);
    Q_INVOKABLE void endTrim();

    // ---- Discrete edits, one undo step each. ----
    // Places a clip of `mediaId` on `trackId` at `startSeconds` (AddClip with
    // ripple=true, so a drop over an occupied span makes room rather than
    // covering it). With snap on the start lands on the frame grid, exactly
    // as a dragged clip does; without it, the start is taken as sent. Returns
    // whether the clip actually placed.
    Q_INVOKABLE bool addClipAt(const QString &mediaId, const QString &trackId, double startSeconds);
    Q_INVOKABLE void removeClips(const QVariantList &clipIds);
    Q_INVOKABLE void removeSelected();
    // Cuts the selected clip in two (SplitClips). The head keeps the id and
    // the tail is minted fresh. The cut lands at `playheadSeconds` when that
    // falls strictly inside the clip; otherwise it falls back to the clip's
    // midpoint, so pressing Split with the playhead parked at the clip's start
    // (the default) still cuts instead of silently doing nothing. Returns
    // whether a cut happened. This is the beginner's "cut" - the only UI path
    // to SplitClips.
    Q_INVOKABLE bool splitSelectedClips(double playheadSeconds);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

    // ---- Inspector edits, one undo step each. ----
    // The clip's display name (UpdateClip with a name-only patch; the clip
    // name lives on the clip, so this is the verb - RenameTimeline is the
    // timeline tab's label, a different entity).
    Q_INVOKABLE void renameClip(const QString &clipId, const QString &name);
    // The picture's placement: all six transform fields, sent absolute so an
    // inspector edit is idempotent (SetClipTransform; the command clamps).
    Q_INVOKABLE void setClipTransform(const QString &clipId, double scale, double offsetX,
                                      double offsetY, double rotation, double stretchX,
                                      double stretchY);
    // Blend strength over what is beneath (UpdateClip with an opacity patch).
    Q_INVOKABLE void setClipOpacity(const QString &clipId, double opacity);
    // Playback rate, holding the source covered constant (SetClipSpeed, which
    // clamps to the engine range 0.0625..=16).
    Q_INVOKABLE void setClipSpeed(const QString &clipId, double speed);
    // Playback direction: true shows the covered source span in reverse
    // (SetClipReverse; the app generates a reversed copy at build time).
    Q_INVOKABLE void setClipReverse(const QString &clipId, bool reverse);
    // Audio ramp lengths in seconds (UpdateClip with a fade patch; the
    // command takes Rational, floored at zero).
    Q_INVOKABLE void setClipFades(const QString &clipId, double fadeIn, double fadeOut);
    // Stereo position of the clip's sound: -1 full left, +1 full right, 0
    // centred (SetClipPan; the command clamps to -1..=1).
    Q_INVOKABLE void setClipPan(const QString &clipId, double pan);
    // Paints one brush stroke onto a clip's cutout (AddCutoutStroke; the
    // command tidies the stroke, turns an automatic cutout custom and creates
    // one where none exists). `tool` is the BrushTool ordinal (0 SmartBrush,
    // 1 Brush, 2 SmartEraser, 3 Eraser); `size` the brush diameter as a
    // fraction of the picture's width; `points` a flat [x0,y0,x1,y1,...] list
    // of source fractions; `at` the source instant in seconds, negative leaves
    // it absent (a plain disc). A stroke with fewer than two points is dropped.
    Q_INVOKABLE void addCutoutStroke(const QString &clipId, int tool, double size,
                                     const QVariantList &points, double at);
    // One of a track's two toggles (SetTrackFlag). `flag` is 0 for
    // TrackFlag::Visible and anything else for TrackFlag::Muted.
    Q_INVOKABLE void setTrackFlag(const QString &trackId, int flag, bool value);

    // ---- Selection (UI state; never recorded in history). ----
    Q_INVOKABLE void select(const QString &clipId);
    Q_INVOKABLE void clearSelection();

signals:
    // canUndo / canRedo / dirty changed. The title bar and undo/redo buttons
    // re-read on this.
    void historyChanged();
    // selectedClipId changed. The strip re-reads on this.
    void selectionChanged();
    // snapEnabled / rippleEnabled changed. The tool toggle buttons re-read.
    void toolsChanged();
    // The timeline's structure changed: the shell rebuilds its projection and
    // reloads the engine. Emitted once per edit, at the end of a gesture, not
    // on every live update of a drag.
    void projectEdited();
    // A brush stroke was committed onto `clipId`'s cutout. The cutout controller
    // re-runs mask generation so the stroke's refinement lands on disk.
    void cutoutStrokeAdded(const QString &clipId);

private:
    enum class Gesture { None, Move, Trim };

    // Ends any in-flight gesture, committing it as its one undo step. Does
    // not refresh: the caller (gesture end or the discrete edit that follows)
    // refreshes exactly once.
    void finishGesture();

    // Applies one discrete command (one undo step) and notifies. Returns
    // whether the command actually applied, so a caller (a stroke commit) can
    // react to a refused edit rather than assuming it landed.
    bool applyEdit(const genesis::project::Command &command);

    genesis::project::Editor *editor_;
    QString selectedClipId_;

    Gesture gesture_ = Gesture::None;
    QString gestureClipId_;
    int gestureEdge_ = 0;
    std::string gestureName_;
    bool gestureApplied_ = false;

    // Timeline tool state. Snap defaults on: a beginner's first drags land on
    // clip edges and the playhead instead of drifting, and the toggle turns it
    // off for frame-exact placement. Ripple stays off - closing gaps silently
    // is a surprise a beginner has not asked for.
    bool snapEnabled_ = true;
    bool rippleEnabled_ = false;
};
