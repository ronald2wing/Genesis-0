// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/controllers/EditController.h"

#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/command/PropertyCommands.h"
#include "project/command/TrackCommands.h"
#include "project/model/Clip.h"
#include "project/model/Timeline.h"
#include "project/model/VideoSettings.h"
#include "project/Editor.h"

#include <cmath>
#include <string_view>

namespace {

using genesis::core::Rational;
using namespace genesis::project;

// The UI -> host seam: seconds (a QML double) become the host's exact
// rational. A value the rational cannot hold (non-finite, out of the
// microseconds range) falls back to zero, matching the command layer's
// clamp-not-error posture.
Rational seconds(double value)
{
    if (const auto rational = Rational::approximate(value)) {
        return *rational;
    }
    return Rational::ZERO;
}

// The QML-facing edge int -> the host enum. Anything but 0 means the tail.
genesis::project::TrimEdge to_edge(int edge)
{
    return edge == 0 ? genesis::project::TrimEdge::Start : genesis::project::TrimEdge::End;
}

// The QML-facing brush int -> the host brush enum, in BrushTool declaration
// order (0 SmartBrush, 1 Brush, 2 SmartEraser, 3 Eraser). Anything else means
// the default smart brush.
genesis::project::BrushTool to_brush_tool(int tool)
{
    switch (tool) {
    case 1:
        return genesis::project::BrushTool::Brush;
    case 2:
        return genesis::project::BrushTool::SmartEraser;
    case 3:
        return genesis::project::BrushTool::Eraser;
    default:
        return genesis::project::BrushTool::SmartBrush;
    }
}

// Quantises a timeline position to the nearest frame boundary of the active
// timeline's output rate. One frame lasts rate_den / rate_num seconds, so a
// dragged start lands on the frame whose leading edge is nearest, rounded
// half-up. Kept in C++ so the exact rational arithmetic lives next to the
// command seam and the QML never re-derives the grid (R4/R5: rational time
// is the host's, not the UI's).
Rational snap_to_frame(Rational value, const VideoSettings &video)
{
    const Rational frame{ video.rate_den, video.rate_num };
    if (frame <= Rational::ZERO) {
        return value;
    }
    const Rational count = (value / frame) + Rational{ 1, 2 };
    return Rational::from_int(count.floor()) * frame;
}

// The pixel radius within which a dragged clip edge is pulled onto a nearby
// edge. Small enough that a deliberate drag wins, wide enough to catch a
// pointer that stopped a few pixels off.
constexpr double kSnapPx = 8.0;

// Snaps a dragged clip's start, when a snap target is within kSnapPx of either
// edge: another clip's start or end on the same track, the playhead, or the
// timeline start. Either the leading or the trailing edge may catch; the
// returned start is the adjustment that lands the caught edge exactly on its
// target. With no target in reach the start falls back to the frame grid, so
// a snapped drag still lands on whole frames.
Rational snap_move_start(const Timeline &timeline, std::string_view clipId, double desiredStart,
                         double playhead, double secsPerPx, const VideoSettings &video)
{
    const Clip *moving = timeline.clip(clipId);
    if (!moving) {
        return snap_to_frame(seconds(desiredStart), video);
    }
    const double duration = moving->duration.as_double();
    const double threshold = secsPerPx > 0.0 ? kSnapPx * secsPerPx : 0.0;
    double start = desiredStart;
    double closest = threshold;
    const auto consider = [&](double target) {
        for (const double edge : { desiredStart, desiredStart + duration }) {
            const double delta = target - edge;
            if (std::abs(delta) < closest) {
                closest = std::abs(delta);
                start = desiredStart + delta;
            }
        }
    };
    // Only the same lane's clips are neighbours; a clip on another track is
    // not something the dragged one can butt against.
    for (const Clip &other : timeline.clips) {
        if (other.id == moving->id || other.track_id != moving->track_id) {
            continue;
        }
        consider(other.start.as_double());
        consider((other.start + other.duration).as_double());
    }
    consider(playhead);
    consider(0.0);
    if (closest < threshold) {
        return seconds(start);
    }
    return snap_to_frame(seconds(desiredStart), video);
}

} // namespace

EditController::EditController(genesis::project::Editor *editor, QObject *parent)
    : QObject(parent), editor_(editor)
{
}

bool EditController::canUndo() const
{
    return editor_ && editor_->can_undo();
}
bool EditController::canRedo() const
{
    return editor_ && editor_->can_redo();
}
bool EditController::dirty() const
{
    return editor_ && editor_->dirty();
}
QString EditController::selectedClipId() const
{
    return selectedClipId_;
}

void EditController::setSnapEnabled(bool value)
{
    if (snapEnabled_ == value) {
        return;
    }
    snapEnabled_ = value;
    emit toolsChanged();
}

void EditController::setRippleEnabled(bool value)
{
    if (rippleEnabled_ == value) {
        return;
    }
    rippleEnabled_ = value;
    emit toolsChanged();
}

void EditController::beginMove(const QString &clipId)
{
    finishGesture();
    if (clipId.isEmpty()) {
        return;
    }
    gesture_ = Gesture::Move;
    gestureClipId_ = clipId;
    gestureName_ = "move:" + clipId.toStdString();
    gestureApplied_ = false;
}

void EditController::updateMove(const QString &clipId, double newStart, double playhead,
                                double secsPerPx)
{
    if (!editor_ || gesture_ != Gesture::Move || clipId != gestureClipId_) {
        return;
    }
    // MoveClips takes an absolute start, so a coalesced gesture is naturally
    // idempotent: the last update's command already describes the final
    // position, and undo/redo land exactly at the gesture's ends. With snap
    // on, a dragged edge within a small pixel radius is pulled onto a
    // neighbour, the playhead or the timeline start, and otherwise onto the
    // frame grid - all before the value becomes a command, so snapping is a
    // pre-commit adjustment, not an edit of its own.
    const Timeline &timeline = editor_->project().active();
    const Rational start = snapEnabled_ ? snap_move_start(timeline, clipId.toStdString(), newStart,
                                                          playhead, secsPerPx, timeline.video)
                                        : seconds(newStart);
    MoveClips move;
    move.moves.push_back(ClipMove{ clipId.toStdString(), start, std::string{ } });
    const auto result = editor_->apply_within(gestureName_, Command{ std::move(move) });
    if (result && result->applied) {
        gestureApplied_ = true;
        emit historyChanged();
    }
}

void EditController::endMove()
{
    if (gesture_ != Gesture::Move) {
        return;
    }
    const bool applied = gestureApplied_;
    finishGesture();
    if (applied) {
        emit projectEdited();
    }
}

void EditController::beginTrim(const QString &clipId, int edge)
{
    finishGesture();
    if (clipId.isEmpty()) {
        return;
    }
    gesture_ = Gesture::Trim;
    gestureClipId_ = clipId;
    gestureEdge_ = edge;
    gestureName_ = "trim:" + clipId.toStdString() + ":" + std::to_string(edge);
    gestureApplied_ = false;
}

void EditController::updateTrim(const QString &clipId, int edge, double delta)
{
    if (!editor_ || gesture_ != Gesture::Trim || clipId != gestureClipId_ || edge != gestureEdge_) {
        return;
    }
    // TrimClip::delta is incremental, but a coalesced gesture must end holding
    // the full cumulative delta so redo replays the whole drag rather than
    // only its last increment. Step the gesture back to where it began, then
    // re-apply the cumulative delta as one coalescing step.
    if (gestureApplied_) {
        editor_->undo();
    }

    TrimClip trim;
    trim.clip_id = clipId.toStdString();
    trim.edge = to_edge(edge);
    trim.delta = seconds(delta);
    trim.ripple = rippleEnabled_;
    const auto result = editor_->apply_within(gestureName_, Command{ std::move(trim) });
    if (result && result->applied) {
        gestureApplied_ = true;
        emit historyChanged();
    }
}

void EditController::endTrim()
{
    if (gesture_ != Gesture::Trim) {
        return;
    }
    const bool applied = gestureApplied_;
    finishGesture();
    if (applied) {
        emit projectEdited();
    }
}

void EditController::removeClips(const QVariantList &clipIds)
{
    RemoveClips remove;
    for (const QVariant &id : clipIds) {
        if (id.isValid() && !id.toString().isEmpty()) {
            remove.clip_ids.push_back(id.toString().toStdString());
        }
    }
    applyEdit(Command{ std::move(remove) });
}

void EditController::removeSelected()
{
    if (selectedClipId_.isEmpty()) {
        return;
    }
    const QString id = selectedClipId_;
    selectedClipId_.clear();
    emit selectionChanged();
    RemoveClips remove;
    remove.clip_ids.push_back(id.toStdString());
    applyEdit(Command{ std::move(remove) });
}

void EditController::undo()
{
    if (!editor_ || !editor_->can_undo()) {
        return;
    }
    finishGesture();
    editor_->undo();
    emit historyChanged();
    emit projectEdited();
}

void EditController::redo()
{
    if (!editor_ || !editor_->can_redo()) {
        return;
    }
    finishGesture();
    editor_->redo();
    emit historyChanged();
    emit projectEdited();
}

void EditController::select(const QString &clipId)
{
    if (selectedClipId_ == clipId) {
        return;
    }
    selectedClipId_ = clipId;
    emit selectionChanged();
}

void EditController::clearSelection()
{
    if (selectedClipId_.isEmpty()) {
        return;
    }
    selectedClipId_.clear();
    emit selectionChanged();
}

void EditController::renameClip(const QString &clipId, const QString &name)
{
    ClipPatch patch;
    patch.name = name.toStdString();
    UpdateClip update;
    update.clip_id = clipId.toStdString();
    update.patch = std::move(patch);
    applyEdit(Command{ std::move(update) });
}

void EditController::setClipTransform(const QString &clipId, double scale, double offsetX,
                                      double offsetY, double rotation, double stretchX,
                                      double stretchY)
{
    SetClipTransform transform;
    transform.clip_id = clipId.toStdString();
    transform.scale = scale;
    transform.offset_x = offsetX;
    transform.offset_y = offsetY;
    transform.rotation = rotation;
    transform.stretch_x = stretchX;
    transform.stretch_y = stretchY;
    applyEdit(Command{ std::move(transform) });
}

void EditController::setClipOpacity(const QString &clipId, double opacity)
{
    ClipPatch patch;
    patch.opacity = opacity;
    UpdateClip update;
    update.clip_id = clipId.toStdString();
    update.patch = std::move(patch);
    applyEdit(Command{ std::move(update) });
}

void EditController::setClipSpeed(const QString &clipId, double speed)
{
    SetClipSpeed change;
    change.clip_id = clipId.toStdString();
    change.speed = speed;
    applyEdit(Command{ std::move(change) });
}

void EditController::setClipReverse(const QString &clipId, bool reverse)
{
    SetClipReverse change;
    change.clip_id = clipId.toStdString();
    change.reverse = reverse;
    applyEdit(Command{ std::move(change) });
}

void EditController::setClipFades(const QString &clipId, double fadeIn, double fadeOut)
{
    ClipPatch patch;
    patch.fade_in = seconds(fadeIn);
    patch.fade_out = seconds(fadeOut);
    UpdateClip update;
    update.clip_id = clipId.toStdString();
    update.patch = std::move(patch);
    applyEdit(Command{ std::move(update) });
}

void EditController::setClipPan(const QString &clipId, double pan)
{
    SetClipPan change;
    change.clip_id = clipId.toStdString();
    change.pan = pan;
    applyEdit(Command{ std::move(change) });
}

void EditController::addCutoutStroke(const QString &clipId, int tool, double size,
                                     const QVariantList &points, double at)
{
    // A stroke needs at least two points to draw a segment; an odd flat list
    // means a half-point that cannot name a source position, so drop the
    // gesture. The command layer tidies the rest (clamps size/points and drops
    // an empty stroke) rather than the controller duplicating it.
    if (clipId.isEmpty() || points.size() < 2 || (points.size() % 2) != 0) {
        return;
    }
    AddCutoutStroke add;
    add.clip_id = clipId.toStdString();
    add.stroke.tool = to_brush_tool(tool);
    add.stroke.size = size;
    add.stroke.points.reserve(static_cast<std::size_t>(points.size()) / 2);
    for (int i = 0; i + 1 < points.size(); i += 2) {
        add.stroke.points.push_back({ points[i].toDouble(), points[i + 1].toDouble() });
    }
    // A non-negative instant names the source frame a smart brush reads; a
    // negative one leaves it absent so the stroke paints as a plain disc.
    if (at >= 0.0) {
        add.stroke.at = seconds(at);
    }
    if (applyEdit(Command{ std::move(add) })) {
        emit cutoutStrokeAdded(clipId);
    }
}

void EditController::setTrackFlag(const QString &trackId, int flag, bool value)
{
    SetTrackFlag change;
    change.track_id = trackId.toStdString();
    change.flag = flag == 0 ? TrackFlag::Visible : TrackFlag::Muted;
    change.value = value;
    applyEdit(Command{ std::move(change) });
}

void EditController::finishGesture()
{
    if (gesture_ == Gesture::None) {
        return;
    }
    gesture_ = Gesture::None;
    gestureClipId_.clear();
    gestureEdge_ = 0;
    gestureName_.clear();
    gestureApplied_ = false;
    if (editor_) {
        editor_->end_gesture();
    }
}

bool EditController::applyEdit(const genesis::project::Command &command)
{
    finishGesture();
    if (!editor_) {
        return false;
    }
    const auto result = editor_->apply(command);
    if (!result || !result->applied) {
        return false;
    }
    emit historyChanged();
    emit projectEdited();
    return true;
}
