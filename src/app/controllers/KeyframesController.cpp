// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/controllers/KeyframesController.h"

#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <QVariantMap>

#include "project/command/Command.h"
#include "project/command/PropertyCommands.h"
#include "project/model/Clip.h"
#include "project/model/Keyframe.h"
#include "project/Editor.h"

namespace {

using namespace genesis::project;

// The refusal sentences, surfaced verbatim to the pane.
const QString kNoClip = QStringLiteral("That clip no longer exists.");
const QString kNoProperty = QStringLiteral("That property cannot be keyed.");
const QString kNotFinite = QStringLiteral("A number in that edit is not finite.");
const QString kNoKey = QStringLiteral("There is no key there.");

// The property name QML passes -> the host enum; nullopt for an unknown name.
std::optional<KeyProperty> to_property(const QString &name)
{
    return from_name(name.toStdString());
}

// A preset name -> its KeyEase. Anything unrecognised (including "custom",
// which names a hand-drawn bezier this controller does not edit) reads as
// linear, matching the model's own fallback.
KeyEase to_ease(const QString &name)
{
    const std::string n = name.toStdString();
    if (n == "in") {
        return KeyEase::in();
    }
    if (n == "out") {
        return KeyEase::out();
    }
    if (n == "inOut") {
        return KeyEase::in_out();
    }
    return KeyEase::linear();
}

// A KeyEase -> the preset name the pane's chips light, "custom" otherwise.
QString ease_name(const KeyEase &ease)
{
    if (ease.is(KeyEase::linear())) {
        return QStringLiteral("linear");
    }
    if (ease.is(KeyEase::in())) {
        return QStringLiteral("in");
    }
    if (ease.is(KeyEase::out())) {
        return QStringLiteral("out");
    }
    if (ease.is(KeyEase::in_out())) {
        return QStringLiteral("inOut");
    }
    return QStringLiteral("custom");
}

// The property's document name as a QString.
QString property_name(KeyProperty property)
{
    return QString::fromStdString(std::string(name(property)));
}

} // namespace

KeyframesController::KeyframesController(genesis::project::Editor *editor, QObject *parent)
    : QObject(parent), editor_(editor)
{
}

void KeyframesController::setClipId(const QString &clipId)
{
    if (clipId_ == clipId) {
        return;
    }
    // A clip change mid-drag commits whatever drag was in flight; the pane's
    // keys are the new clip's from here on.
    finishGesture();
    clipId_ = clipId;
    emit clipIdChanged();
    emit propertiesChanged();
}

void KeyframesController::setPlayheadPosition(double at)
{
    if (playheadPosition_ == at) {
        return;
    }
    playheadPosition_ = at;
    emit playheadChanged();
}

bool KeyframesController::canUndo() const
{
    return editor_ && editor_->can_undo();
}
bool KeyframesController::canRedo() const
{
    return editor_ && editor_->can_redo();
}
bool KeyframesController::dirty() const
{
    return editor_ && editor_->dirty();
}

const genesis::project::Clip *KeyframesController::clip() const
{
    if (!editor_ || clipId_.isEmpty()) {
        return nullptr;
    }
    return editor_->project().active().clip(clipId_.toStdString());
}

QVariantList KeyframesController::keyedProperties() const
{
    QVariantList out;
    const Clip *selected = clip();
    if (selected == nullptr) {
        return out;
    }
    for (const KeyProperty property : key_property_all) {
        if (!selected->is_keyed(property)) {
            continue;
        }
        QVariantMap item;
        item.insert(QStringLiteral("property"), property_name(property));
        out.append(item);
    }
    return out;
}

QVariantList KeyframesController::keysFor(const QString &property) const
{
    QVariantList out;
    const Clip *selected = clip();
    const std::optional<KeyProperty> prop = to_property(property);
    if (selected == nullptr || !prop) {
        return out;
    }
    for (const ClipKey &key : selected->keys_on(*prop)) {
        QVariantMap item;
        item.insert(QStringLiteral("at"), key.at);
        item.insert(QStringLiteral("value"), key.value);
        item.insert(QStringLiteral("ease"), ease_name(key.ease));
        out.append(item);
    }
    return out;
}

QVariantList KeyframesController::sample(const QString &property, double from, double to,
                                         int count) const
{
    QVariantList out;
    const Clip *selected = clip();
    const std::optional<KeyProperty> prop = to_property(property);
    if (selected == nullptr || !prop || count < 2) {
        return out;
    }
    out.reserve(count);
    for (int i = 0; i < count; ++i) {
        const double t =
                from + (to - from) * static_cast<double>(i) / static_cast<double>(count - 1);
        QVariantMap item;
        item.insert(QStringLiteral("at"), t);
        item.insert(QStringLiteral("value"), selected->value_at(*prop, t));
        out.append(item);
    }
    return out;
}

bool KeyframesController::addKey(const QString &property, double at, double value,
                                 const QString &ease)
{
    const Clip *selected = clip();
    if (selected == nullptr) {
        refuse(kNoClip);
        return false;
    }
    const std::optional<KeyProperty> prop = to_property(property);
    if (!prop) {
        refuse(kNoProperty);
        return false;
    }
    if (!std::isfinite(at) || !std::isfinite(value)) {
        refuse(kNotFinite);
        return false;
    }
    SetClipKey set;
    set.clip_id = selected->id;
    set.property = *prop;
    set.at = at;
    set.value = value;
    set.ease = to_ease(ease);
    return applyEdit(Command{ std::move(set) });
}

bool KeyframesController::removeKey(const QString &property, double at)
{
    const Clip *selected = clip();
    if (selected == nullptr) {
        refuse(kNoClip);
        return false;
    }
    const std::optional<KeyProperty> prop = to_property(property);
    if (!prop) {
        refuse(kNoProperty);
        return false;
    }
    if (!std::isfinite(at)) {
        refuse(kNotFinite);
        return false;
    }
    if (!selected->key_at(*prop, at)) {
        refuse(kNoKey);
        return false;
    }
    ClearClipKey clear;
    clear.clip_id = selected->id;
    clear.property = *prop;
    clear.at = at;
    return applyEdit(Command{ std::move(clear) });
}

bool KeyframesController::clearKeys(const QString &property)
{
    const Clip *selected = clip();
    if (selected == nullptr) {
        refuse(kNoClip);
        return false;
    }
    const std::optional<KeyProperty> prop = to_property(property);
    if (!prop) {
        refuse(kNoProperty);
        return false;
    }
    ClearClipKeys clear;
    clear.clip_id = selected->id;
    clear.property = *prop;
    return applyEdit(Command{ std::move(clear) });
}

bool KeyframesController::setKeyEase(const QString &property, double at, const QString &ease)
{
    const Clip *selected = clip();
    if (selected == nullptr) {
        refuse(kNoClip);
        return false;
    }
    const std::optional<KeyProperty> prop = to_property(property);
    if (!prop) {
        refuse(kNoProperty);
        return false;
    }
    if (!std::isfinite(at)) {
        refuse(kNotFinite);
        return false;
    }
    const std::optional<std::size_t> index = selected->key_at(*prop, at);
    if (!index) {
        refuse(kNoKey);
        return false;
    }
    const ClipKey &key = selected->keys[*index];
    SetClipKey set;
    set.clip_id = selected->id;
    set.property = *prop;
    set.at = key.at;
    set.value = key.value;
    set.ease = to_ease(ease);
    return applyEdit(Command{ std::move(set) });
}

bool KeyframesController::beginKeyMove(const QString &property, double at)
{
    finishGesture();
    const Clip *selected = clip();
    if (selected == nullptr) {
        refuse(kNoClip);
        return false;
    }
    const std::optional<KeyProperty> prop = to_property(property);
    if (!prop) {
        refuse(kNoProperty);
        return false;
    }
    if (!std::isfinite(at)) {
        refuse(kNotFinite);
        return false;
    }
    const std::optional<std::size_t> index = selected->key_at(*prop, at);
    if (!index) {
        refuse(kNoKey);
        return false;
    }
    const ClipKey &key = selected->keys[*index];
    gesture_ = Gesture::KeyMove;
    gestureClipId_ = selected->id;
    gestureProperty_ = *prop;
    gestureOriginalAt_ = key.at;
    gestureEase_ = key.ease;
    gestureName_ = "keymove:" + gestureClipId_ + ":" + std::string(name(*prop));
    gestureApplied_ = false;
    return true;
}

void KeyframesController::updateKeyMove(const QString &property, double newAt, double newValue)
{
    if (!editor_ || gesture_ != Gesture::KeyMove) {
        return;
    }
    const std::optional<KeyProperty> prop = to_property(property);
    if (!prop || *prop != gestureProperty_) {
        return;
    }
    if (!std::isfinite(newAt) || !std::isfinite(newValue)) {
        return; // a bad live sample is dropped, not refused
    }
    // A move is "take the key at its original position and put it at the new
    // one", as one atomic batch. Because the batch's before-image is the
    // gesture's start, undo lands there; stepping the previous move back first
    // keeps the batch absolute and idempotent, so redo replays the whole drag
    // and the clip never accumulates duplicate keys along the way.
    if (gestureApplied_) {
        editor_->undo();
    }
    Batch batch;
    batch.commands.push_back(
            Command{ ClearClipKey{ gestureClipId_, gestureProperty_, gestureOriginalAt_ } });
    batch.commands.push_back(Command{
            SetClipKey{ gestureClipId_, gestureProperty_, newAt, newValue, gestureEase_ } });
    const auto result = editor_->apply_within(gestureName_, Command{ std::move(batch) });
    if (result && result->applied) {
        gestureApplied_ = true;
        emit historyChanged();
        emit propertiesChanged();
    }
}

void KeyframesController::endKeyMove()
{
    if (gesture_ != Gesture::KeyMove) {
        return;
    }
    const bool applied = gestureApplied_;
    finishGesture();
    if (applied) {
        emit projectEdited();
    }
}

void KeyframesController::undo()
{
    if (!editor_ || !editor_->can_undo()) {
        return;
    }
    finishGesture();
    editor_->undo();
    emit historyChanged();
    emit propertiesChanged();
    emit projectEdited();
}

void KeyframesController::redo()
{
    if (!editor_ || !editor_->can_redo()) {
        return;
    }
    finishGesture();
    editor_->redo();
    emit historyChanged();
    emit propertiesChanged();
    emit projectEdited();
}

void KeyframesController::refuse(const QString &reason)
{
    if (lastError_ == reason) {
        return;
    }
    lastError_ = reason;
    emit errorChanged();
}

void KeyframesController::clearError()
{
    if (lastError_.isEmpty()) {
        return;
    }
    lastError_.clear();
    emit errorChanged();
}

bool KeyframesController::applyEdit(const genesis::project::Command &command)
{
    finishGesture();
    if (!editor_) {
        return false;
    }
    const auto result = editor_->apply(command);
    if (!result) {
        // The command layer refused (a non-finite value, in practice); its
        // sentence is the contract the pane shows.
        refuse(QString::fromStdString(std::string(result.error().message())));
        return false;
    }
    if (!result->applied) {
        return false; // a tolerated no-op: nothing to say, nothing recorded
    }
    clearError();
    emit historyChanged();
    emit propertiesChanged();
    emit projectEdited();
    return true;
}

void KeyframesController::finishGesture()
{
    if (gesture_ == Gesture::None) {
        return;
    }
    gesture_ = Gesture::None;
    gestureClipId_.clear();
    gestureOriginalAt_ = 0.0;
    gestureEase_ = KeyEase::linear();
    gestureName_.clear();
    gestureApplied_ = false;
    if (editor_) {
        editor_->end_gesture();
    }
}
