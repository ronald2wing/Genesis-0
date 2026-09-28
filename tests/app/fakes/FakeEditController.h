// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// A fake QObject EditController for QML interaction tests. Records every call
// into a QStringList so the test can assert the right QML gesture reached the
// right host method without linking the real controller, timeline, or engine.

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class FakeEditController : public QObject
{
    Q_OBJECT

public:
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY canUndoChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY canRedoChanged)
    Q_PROPERTY(QString selectedClipId READ selectedClipId NOTIFY selectedClipIdChanged)
    Q_PROPERTY(bool snapEnabled READ snapEnabled NOTIFY snapEnabledChanged)
    Q_PROPERTY(bool rippleEnabled READ rippleEnabled NOTIFY rippleEnabledChanged)

    QStringList calls;

    bool canUndo() const { return _canUndo; }
    bool canRedo() const { return _canRedo; }
    QString selectedClipId() const { return _selectedClipId; }
    bool snapEnabled() const { return _snapEnabled; }
    bool rippleEnabled() const { return _rippleEnabled; }

    void setCanUndo(bool v)
    {
        _canUndo = v;
        emit canUndoChanged();
    }
    void setCanRedo(bool v)
    {
        _canRedo = v;
        emit canRedoChanged();
    }
    void setSnapEnabled(bool v)
    {
        _snapEnabled = v;
        emit snapEnabledChanged();
    }
    void setRippleEnabled(bool v)
    {
        _rippleEnabled = v;
        emit rippleEnabledChanged();
    }

    void clearCalls() { calls.clear(); }
    void setSelectedClipId(const QString &id)
    {
        _selectedClipId = id;
        emit selectedClipIdChanged();
    }

    Q_INVOKABLE bool addClipAt(const QString &mediaId, const QString &trackId, double start)
    {
        calls.append(QStringLiteral("addClipAt:%1:%2:%3").arg(mediaId, trackId).arg(start));
        return true;
    }

    Q_INVOKABLE void beginMove(const QString &clipId)
    {
        calls.append(QStringLiteral("beginMove:%1").arg(clipId));
    }

    Q_INVOKABLE void updateMove(const QString &clipId, double pos, double snapPos, double secsPerPx)
    {
        calls.append(QStringLiteral("updateMove:%1:%2:%3:%4")
                             .arg(clipId)
                             .arg(pos)
                             .arg(snapPos)
                             .arg(secsPerPx));
    }

    Q_INVOKABLE void endMove() { calls.append(QStringLiteral("endMove")); }

    Q_INVOKABLE void beginTrim(const QString &clipId, int edge)
    {
        calls.append(QStringLiteral("beginTrim:%1:%2").arg(clipId).arg(edge));
    }

    Q_INVOKABLE void updateTrim(const QString &clipId, int edge, double delta)
    {
        calls.append(QStringLiteral("updateTrim:%1:%2:%3").arg(clipId).arg(edge).arg(delta));
    }

    Q_INVOKABLE void endTrim() { calls.append(QStringLiteral("endTrim")); }

    Q_INVOKABLE void select(const QString &clipId)
    {
        calls.append(QStringLiteral("select:%1").arg(clipId));
        _selectedClipId = clipId;
        emit selectedClipIdChanged();
    }

    Q_INVOKABLE void setTrackFlag(const QString &trackId, int flag, bool value)
    {
        calls.append(QStringLiteral("setTrackFlag:%1:%2:%3")
                             .arg(trackId)
                             .arg(flag)
                             .arg(value ? QStringLiteral("true") : QStringLiteral("false")));
    }

    Q_INVOKABLE void undo() { calls.append(QStringLiteral("undo")); }
    Q_INVOKABLE void redo() { calls.append(QStringLiteral("redo")); }
    Q_INVOKABLE void removeSelected() { calls.append(QStringLiteral("removeSelected")); }

signals:
    void canUndoChanged();
    void canRedoChanged();
    void selectedClipIdChanged();
    void snapEnabledChanged();
    void rippleEnabledChanged();

private:
    bool _canUndo = true;
    bool _canRedo = true;
    QString _selectedClipId;
    bool _snapEnabled = false;
    bool _rippleEnabled = false;
};