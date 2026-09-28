// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/controllers/HardwareStatus.h"

HardwareStatus::HardwareStatus(QObject *parent) : QObject(parent) { }

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

void HardwareStatus::setProbe(const QString &renderer, bool softwareGl, bool hardwareDecode,
                              bool hardwareEncode, const QString &encodeElement)
{
    renderer_ = renderer;
    softwareGl_ = softwareGl;
    hardwareDecode_ = hardwareDecode;
    hardwareEncode_ = hardwareEncode;
    encodeElement_ = encodeElement;
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
