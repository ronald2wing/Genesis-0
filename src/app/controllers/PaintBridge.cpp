// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/controllers/PaintBridge.h"

PaintBridge::PaintBridge(QObject *parent) : QObject(parent) { }

bool PaintBridge::active() const
{
    return active_;
}

QString PaintBridge::clipId() const
{
    return clip_id_;
}

void PaintBridge::setActive(bool active)
{
    if (active_ == active) {
        return;
    }
    active_ = active;
    emit changed();
}

void PaintBridge::setClipId(const QString &clipId)
{
    if (clip_id_ == clipId) {
        return;
    }
    clip_id_ = clipId;
    emit changed();
}
