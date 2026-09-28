// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/controllers/HardwareStatus.h"

#include <string>

#include "adapters/engine/ges/export/Hardware.h"

HardwareStatus::HardwareStatus(QObject *parent) : QObject(parent) { }

QString HardwareStatus::nvidiaHint() const
{
    const std::string renderer = renderer_.toStdString();
    return QString::fromStdString(
            genesis::adapters::engine::ges::nvidia_offload_hint(renderer, nvidiaAvailable_));
}

QString HardwareStatus::decode() const
{
    // Hardware decode is in use when it is available and the user has not
    // forced software (preference 1). Auto (0) lets the engine rank, which
    // prefers hardware when present; Hardware (2) demands it and falls back to
    // software when no hardware decoder exists.
    if (hardwareDecode_ && decodePreference_ != 1) {
        return QStringLiteral("hardware");
    }
    return QStringLiteral("software");
}

QString HardwareStatus::decodePolicy() const
{
    switch (decodePreference_) {
    case 1:
        return QStringLiteral("software");
    case 2:
        return QStringLiteral("hardware");
    default:
        return QStringLiteral("auto");
    }
}

void HardwareStatus::setProbe(const QString &renderer, bool softwareGl, bool hardwareDecode,
                              bool hardwareEncode, const QString &encodeElement,
                              bool nvidiaAvailable, const QStringList &nvidiaElements)
{
    renderer_ = renderer;
    softwareGl_ = softwareGl;
    hardwareDecode_ = hardwareDecode;
    hardwareEncode_ = hardwareEncode;
    encodeElement_ = encodeElement;
    nvidiaAvailable_ = nvidiaAvailable;
    nvidiaElements_ = nvidiaElements;
    emit changed();
}

void HardwareStatus::setDecodePreference(int value)
{
    if (value < 0 || value > 2 || value == decodePreference_) {
        return;
    }
    decodePreference_ = value;
    emit changed();
}

void HardwareStatus::setAi(const QString &mode)
{
    if (mode == ai_) {
        return;
    }
    ai_ = mode;
    emit changed();
}
