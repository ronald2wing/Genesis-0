// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/controllers/EffectParamsController.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QVariantMap>

#include "effects/Pack.h"
#include "project/command/Command.h"
#include "project/command/PropertyCommands.h"
#include "project/model/Clip.h"
#include "project/Editor.h"
#include "render/Catalogue.h"

namespace {

using namespace genesis::project;

// The refusal sentences, surfaced verbatim to the pane.
const QString kNoClip = QStringLiteral("That clip no longer exists.");
const QString kNoEffect = QStringLiteral("That effect is no longer on the clip.");
const QString kNotFinite = QStringLiteral("A number in that edit is not finite.");
const QString kBadCurve = QStringLiteral("That curve is not a list of x/y points.");
const QString kTooManyCurvePoints = QStringLiteral("That curve has too many points.");

// A ParamType -> the token the pane maps to a control ("float"/"int"/...).
QString type_name(genesis::effects::ParamType type)
{
    return QString::fromLatin1(genesis::effects::name(type));
}

// Packs four 0..1 channels into the 32-bit RGBA the resolver unpacks: R in
// the high byte, then G, B, A - the same packing `params_block` reads back.
std::uint32_t pack_rgba(double r, double g, double b, double a)
{
    const auto byte = [](double channel) {
        const double clamped = std::clamp(channel, 0.0, 1.0);
        return static_cast<std::uint32_t>(std::llround(clamped * 255.0)) & 0xffu;
    };
    return (byte(r) << 24) | (byte(g) << 16) | (byte(b) << 8) | byte(a);
}

// One 0..1 channel out of the packed RGBA, matching the resolver's bounds.
double channel_of(double packed, int shift)
{
    if (!std::isfinite(packed) || packed <= 0.0) {
        return 0.0;
    }
    const std::uint32_t value =
            packed >= 4294967295.0 ? 0xffffffffu : static_cast<std::uint32_t>(packed);
    return static_cast<double>((value >> shift) & 0xffu) / 255.0;
}

// A curve's points, clamped into the unit square, x-sorted, and de-duplicated
// (same x keeps the later) - the canonical form both the resolver and the
// setter agree on.
std::vector<std::pair<double, double>>
normalize_curve_points(std::vector<std::pair<double, double>> points)
{
    for (auto &[x, y] : points) {
        x = std::clamp(x, 0.0, 1.0);
        y = std::clamp(y, 0.0, 1.0);
    }
    std::stable_sort(points.begin(), points.end(),
                     [](const auto &a, const auto &b) { return a.first < b.first; });
    std::vector<std::pair<double, double>> kept;
    for (const auto &point : points) {
        if (!kept.empty() && std::abs(point.first - kept.back().first) < 1e-6) {
            kept.back() = point;
        } else {
            kept.push_back(point);
        }
    }
    return kept;
}

// The curve parameter's current points at the clip start, read the way the
// resolver reads them: a slot holds a point only when both its x and y are
// set (as a constant or a key); fewer than two is the identity line.
std::vector<std::pair<double, double>> curve_points_of(const AppliedFilter &instance,
                                                       std::string_view key)
{
    constexpr double kAbsent = std::numeric_limits<double>::quiet_NaN();
    std::vector<std::pair<double, double>> points;
    for (std::size_t n = 0; n < genesis::effects::MAX_CURVE_POINTS; ++n) {
        const std::string prefix = std::string(key) + "." + std::to_string(n) + ".";
        const double x = instance.value_at(prefix + "x", 0.0, kAbsent);
        const double y = instance.value_at(prefix + "y", 0.0, kAbsent);
        if (!std::isfinite(x) || !std::isfinite(y)) {
            continue;
        }
        points.emplace_back(x, y);
    }
    points = normalize_curve_points(std::move(points));
    if (points.size() < 2) {
        return { { 0.0, 0.0 }, { 1.0, 1.0 } };
    }
    return points;
}

// A SetEffectKey command: one key at the clip start (the static edit the
// controller's header note documents).
Command effect_key(const std::string &clip_id, std::size_t entry, std::string key, double value)
{
    SetEffectKey set;
    set.clip_id = clip_id;
    set.entry = entry;
    set.key = std::move(key);
    set.at = 0.0;
    set.value = value;
    set.ease = KeyEase::linear();
    return Command{ std::move(set) };
}

// A ClearEffectKeys command for one dotted key, so a shrinking compound value
// drops its stale slots.
Command effect_clear(const std::string &clip_id, std::size_t entry, std::string key)
{
    ClearEffectKeys clear;
    clear.clip_id = clip_id;
    clear.entry = entry;
    clear.key = std::move(key);
    return Command{ std::move(clear) };
}

// One declared parameter as a QVariantMap, including its current value at the
// clip start (the whole-clip value under the static representation). The
// instance's value comes from the model's own value_at, so the pane never
// re-derives the ride from the raw params/keys. The compound types add their
// component fields beside `value` (see the header's projection note).
QVariantMap param_map(const genesis::effects::Parameter &parameter, const AppliedFilter &instance)
{
    QVariantMap out;
    out.insert(QStringLiteral("key"), QString::fromStdString(parameter.key));
    out.insert(QStringLiteral("label"), QString::fromStdString(parameter.label));
    out.insert(QStringLiteral("group"), QString::fromStdString(parameter.group));
    out.insert(QStringLiteral("type"), type_name(parameter.type));
    out.insert(QStringLiteral("min"), parameter.min);
    out.insert(QStringLiteral("max"), parameter.max);
    out.insert(QStringLiteral("step"), parameter.step);
    out.insert(QStringLiteral("unit"), QString::fromStdString(parameter.unit));
    out.insert(QStringLiteral("animate"), parameter.animate);
    out.insert(QStringLiteral("defaultValue"), parameter.default_value);
    const double value = instance.value_at(parameter.key, 0.0, parameter.default_value);
    out.insert(QStringLiteral("value"), value);

    switch (parameter.type) {
    case genesis::effects::ParamType::Color:
        out.insert(QStringLiteral("r"), channel_of(value, 24));
        out.insert(QStringLiteral("g"), channel_of(value, 16));
        out.insert(QStringLiteral("b"), channel_of(value, 8));
        out.insert(QStringLiteral("a"), channel_of(value, 0));
        break;
    case genesis::effects::ParamType::Point:
        out.insert(QStringLiteral("x"), instance.value_at(parameter.key + ".x", 0.0, 0.5));
        out.insert(QStringLiteral("y"), instance.value_at(parameter.key + ".y", 0.0, 0.5));
        break;
    case genesis::effects::ParamType::Wheel:
        out.insert(QStringLiteral("x"), instance.value_at(parameter.key + ".x", 0.0, 0.0));
        out.insert(QStringLiteral("y"), instance.value_at(parameter.key + ".y", 0.0, 0.0));
        out.insert(QStringLiteral("m"),
                   instance.value_at(parameter.key + ".m", 0.0, parameter.default_value));
        break;
    case genesis::effects::ParamType::Curve: {
        QVariantList points;
        for (const auto &[x, y] : curve_points_of(instance, parameter.key)) {
            QVariantMap point;
            point.insert(QStringLiteral("x"), x);
            point.insert(QStringLiteral("y"), y);
            points.append(point);
        }
        out.insert(QStringLiteral("points"), points);
        break;
    }
    case genesis::effects::ParamType::Float:
    case genesis::effects::ParamType::Int:
    case genesis::effects::ParamType::Bool:
    case genesis::effects::ParamType::Enum:
        break;
    }

    // The enum's choices: `values` in the manifest's own order, `labels`
    // beside them. The label list is the whole point of the projection - the
    // LUT look names rather than their integer indices.
    QVariantList values;
    QVariantList labels;
    for (const double value : parameter.values) {
        values.append(value);
    }
    for (const std::string &label : parameter.labels) {
        labels.append(QString::fromStdString(label));
    }
    out.insert(QStringLiteral("values"), values);
    out.insert(QStringLiteral("labels"), labels);
    return out;
}

} // namespace

EffectParamsController::EffectParamsController(genesis::project::Editor *editor, QObject *parent,
                                               const genesis::render::Catalogue *catalogue)
    : QObject(parent),
      editor_(editor),
      catalogue_(catalogue ? catalogue : &genesis::render::Catalogue::builtin())
{
}

void EffectParamsController::setClipId(const QString &clipId)
{
    if (clipId_ == clipId) {
        return;
    }
    clipId_ = clipId;
    emit clipIdChanged();
    emit effectsChanged();
}

bool EffectParamsController::canUndo() const
{
    return editor_ && editor_->can_undo();
}
bool EffectParamsController::canRedo() const
{
    return editor_ && editor_->can_redo();
}
bool EffectParamsController::dirty() const
{
    return editor_ && editor_->dirty();
}

const genesis::project::Clip *EffectParamsController::clip() const
{
    if (!editor_ || clipId_.isEmpty()) {
        return nullptr;
    }
    return editor_->project().active().clip(clipId_.toStdString());
}

const genesis::project::Clip *EffectParamsController::target(int entry)
{
    const Clip *selected = clip();
    if (selected == nullptr) {
        refuse(kNoClip);
        return nullptr;
    }
    if (entry < 0 || static_cast<std::size_t>(entry) >= selected->video_effects.size()) {
        refuse(kNoEffect);
        return nullptr;
    }
    return selected;
}

QVariantList EffectParamsController::effects() const
{
    QVariantList out;
    const Clip *selected = clip();
    if (selected == nullptr) {
        return out;
    }
    for (std::size_t entry = 0; entry < selected->video_effects.size(); ++entry) {
        const AppliedFilter &instance = selected->video_effects[entry];
        QVariantMap link;
        link.insert(QStringLiteral("entry"), static_cast<int>(entry));
        link.insert(QStringLiteral("id"), QString::fromStdString(instance.id));
        link.insert(QStringLiteral("enabled"), instance.enabled);

        const genesis::effects::Pack *pack = catalogue_->pack(instance.id);
        QVariantList parameters;
        if (pack != nullptr) {
            link.insert(QStringLiteral("name"), QString::fromStdString(pack->name));
            parameters.reserve(pack->parameters.size());
            for (const genesis::effects::Parameter &parameter : pack->parameters) {
                parameters.append(param_map(parameter, instance));
            }
        } else {
            // An id the catalogue no longer knows: still reported (id +
            // enabled) so the pane shows the link exists, with no parameters
            // to edit - the resolver would skip it anyway (R9).
            link.insert(QStringLiteral("name"), QString());
        }
        link.insert(QStringLiteral("parameters"), parameters);
        out.append(link);
    }
    return out;
}

bool EffectParamsController::setParameter(int entry, const QString &key, double value)
{
    const Clip *selected = target(entry);
    if (selected == nullptr) {
        return false;
    }
    if (!std::isfinite(value)) {
        refuse(kNotFinite);
        return false;
    }
    return applyEdit(
            effect_key(selected->id, static_cast<std::size_t>(entry), key.toStdString(), value));
}

bool EffectParamsController::setColor(int entry, const QString &key, double r, double g, double b,
                                      double a)
{
    const Clip *selected = target(entry);
    if (selected == nullptr) {
        return false;
    }
    if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b) || !std::isfinite(a)) {
        refuse(kNotFinite);
        return false;
    }
    return applyEdit(effect_key(selected->id, static_cast<std::size_t>(entry), key.toStdString(),
                                static_cast<double>(pack_rgba(r, g, b, a))));
}

bool EffectParamsController::setPoint(int entry, const QString &key, double x, double y)
{
    const Clip *selected = target(entry);
    if (selected == nullptr) {
        return false;
    }
    if (!std::isfinite(x) || !std::isfinite(y)) {
        refuse(kNotFinite);
        return false;
    }
    const std::string base = key.toStdString();
    return applyBatch({
            effect_key(selected->id, static_cast<std::size_t>(entry), base + ".x", x),
            effect_key(selected->id, static_cast<std::size_t>(entry), base + ".y", y),
    });
}

bool EffectParamsController::setWheel(int entry, const QString &key, double x, double y, double m)
{
    const Clip *selected = target(entry);
    if (selected == nullptr) {
        return false;
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(m)) {
        refuse(kNotFinite);
        return false;
    }
    const std::string base = key.toStdString();
    return applyBatch({
            effect_key(selected->id, static_cast<std::size_t>(entry), base + ".x", x),
            effect_key(selected->id, static_cast<std::size_t>(entry), base + ".y", y),
            effect_key(selected->id, static_cast<std::size_t>(entry), base + ".m", m),
    });
}

bool EffectParamsController::setCurve(int entry, const QString &key, const QVariantList &points)
{
    const Clip *selected = target(entry);
    if (selected == nullptr) {
        return false;
    }

    std::vector<std::pair<double, double>> raw;
    raw.reserve(static_cast<std::size_t>(points.size()));
    for (const QVariant &element : points) {
        const QVariantMap point = element.toMap();
        const QVariant x = point.value(QStringLiteral("x"));
        const QVariant y = point.value(QStringLiteral("y"));
        if (!x.canConvert<double>() || !y.canConvert<double>()) {
            refuse(kBadCurve);
            return false;
        }
        const double px = x.toDouble();
        const double py = y.toDouble();
        if (!std::isfinite(px) || !std::isfinite(py)) {
            refuse(kNotFinite);
            return false;
        }
        raw.emplace_back(px, py);
    }
    const std::vector<std::pair<double, double>> normalized =
            normalize_curve_points(std::move(raw));
    if (normalized.size() > genesis::effects::MAX_CURVE_POINTS) {
        refuse(kTooManyCurvePoints);
        return false;
    }

    const std::string base = key.toStdString();
    std::vector<Command> commands;
    // A SetEffectKey pair per kept point plus a clear pair for every slot
    // beyond the new count, so the batch is at most MAX_CURVE_POINTS pairs.
    commands.reserve(genesis::effects::MAX_CURVE_POINTS * 2);
    for (std::size_t n = 0; n < normalized.size(); ++n) {
        const std::string prefix = base + "." + std::to_string(n) + ".";
        commands.push_back(effect_key(selected->id, static_cast<std::size_t>(entry), prefix + "x",
                                      normalized[n].first));
        commands.push_back(effect_key(selected->id, static_cast<std::size_t>(entry), prefix + "y",
                                      normalized[n].second));
    }
    // Drop the slots the new curve no longer uses, so a shrinking curve never
    // leaves a stale point behind.
    for (std::size_t n = normalized.size(); n < genesis::effects::MAX_CURVE_POINTS; ++n) {
        const std::string prefix = base + "." + std::to_string(n) + ".";
        commands.push_back(
                effect_clear(selected->id, static_cast<std::size_t>(entry), prefix + "x"));
        commands.push_back(
                effect_clear(selected->id, static_cast<std::size_t>(entry), prefix + "y"));
    }
    return applyBatch(std::move(commands));
}

void EffectParamsController::undo()
{
    if (!editor_ || !editor_->can_undo()) {
        return;
    }
    editor_->undo();
    emit historyChanged();
    emit effectsChanged();
    emit projectEdited();
}

void EffectParamsController::redo()
{
    if (!editor_ || !editor_->can_redo()) {
        return;
    }
    editor_->redo();
    emit historyChanged();
    emit effectsChanged();
    emit projectEdited();
}

void EffectParamsController::refuse(const QString &reason)
{
    if (lastError_ == reason) {
        return;
    }
    lastError_ = reason;
    emit errorChanged();
}

void EffectParamsController::clearError()
{
    if (lastError_.isEmpty()) {
        return;
    }
    lastError_.clear();
    emit errorChanged();
}

bool EffectParamsController::applyEdit(const genesis::project::Command &command)
{
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
    emit effectsChanged();
    emit projectEdited();
    return true;
}

bool EffectParamsController::applyBatch(std::vector<genesis::project::Command> commands)
{
    Batch batch;
    batch.commands = std::move(commands);
    return applyEdit(Command{ std::move(batch) });
}
