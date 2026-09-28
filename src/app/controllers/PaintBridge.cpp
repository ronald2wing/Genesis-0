// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/controllers/PaintBridge.h"

PaintBridge::PaintBridge(QObject *parent) : QObject(parent) { }

bool PaintBridge::active() const
{
    return m_active;
}

QString PaintBridge::clipId() const
{
    return m_clipId;
}

void PaintBridge::setActive(bool active)
{
    if (m_active == active) {
        return;
    }
    m_active = active;
    emit changed();
}

void PaintBridge::setClipId(const QString &clipId)
{
    if (m_clipId == clipId) {
        return;
    }
    m_clipId = clipId;
    emit changed();
}
