// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The shell's stroke-painting overlay state, exposed to QML as the `paint`
// context property. The cutout panel toggles it through the `ui.paint` verb
// (which the shell's `Services::ui_paint` seam mirrors onto this object), and
// the shell keeps `clipId` synced to the selection so deselecting a clip hides
// the overlay. The object is a plain state holder: `active` gates the monitor
// overlay, `clipId` says which clip it paints onto (empty for none).

#pragma once

#include <QObject>
#include <QString>

class PaintBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY changed)
    Q_PROPERTY(QString clipId READ clipId WRITE setClipId NOTIFY changed)

public:
    explicit PaintBridge(QObject *parent = nullptr);

    bool active() const;
    QString clipId() const;

public slots:
    void setActive(bool active);
    void setClipId(const QString &clipId);

signals:
    // Raised whenever `active` or `clipId` changes, so QML rebinds cheaply.
    void changed();

private:
    bool m_active = false;
    QString m_clipId;
};
