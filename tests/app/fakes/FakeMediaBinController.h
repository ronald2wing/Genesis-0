// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// A fake QObject MediaBinController for QML interaction tests. Records every
// call into a QStringList and provides the properties QML reads so the
// surfaces can render without the real controller or engine.

#pragma once

#include <QVariant>
#include <QVariantList>
#include <QObject>
#include <QString>
#include <QStringList>

class FakeMediaBinController : public QObject
{
    Q_OBJECT

public:
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
    Q_PROPERTY(int sortMode READ sortMode WRITE setSortMode NOTIFY sortModeChanged)
    Q_PROPERTY(QString selectedMediaId READ selectedMediaId NOTIFY selectedMediaIdChanged)
    Q_PROPERTY(int cacheVersion READ cacheVersion NOTIFY cacheVersionChanged)
    Q_PROPERTY(bool generating READ generating NOTIFY generatingChanged)

    QStringList calls;

    QString searchText() const { return _searchText; }
    void setSearchText(const QString &v)
    {
        _searchText = v;
        emit searchTextChanged();
    }

    int sortMode() const { return _sortMode; }
    void setSortMode(int v)
    {
        _sortMode = v;
        emit sortModeChanged();
    }

    QString selectedMediaId() const { return _selectedMediaId; }
    int cacheVersion() const { return _cacheVersion; }
    bool generating() const { return _generating; }

    void clearCalls() { calls.clear(); }

    Q_INVOKABLE QVariantList apply(const QVariantList &items, const QString & /*query*/,
                                   int /*sortMode*/)
    {
        return items;
    }

    Q_INVOKABLE QString posterSource(const QString & /*id*/, int /*version*/) { return QString(); }
    Q_INVOKABLE QVariantList waveformPeaks(const QString & /*id*/, int /*version*/) { return { }; }

    Q_INVOKABLE void select(const QString &id)
    {
        calls.append(QStringLiteral("select:%1").arg(id));
        _selectedMediaId = id;
        emit selectedMediaIdChanged();
    }

    Q_INVOKABLE void generateWaveform(const QString &id)
    {
        calls.append(QStringLiteral("generateWaveform:%1").arg(id));
    }

    Q_INVOKABLE bool isPlaceholder(const QString & /*id*/) { return false; }

    Q_INVOKABLE int importFiles(const QVariantList & /*urls*/) { return 0; }

    Q_INVOKABLE void relinkMedia(const QString &id, const QString &file)
    {
        calls.append(QStringLiteral("relinkMedia:%1:%2").arg(id, file));
    }

    Q_INVOKABLE void fillSlot(const QString &id, const QString &file)
    {
        calls.append(QStringLiteral("fillSlot:%1:%2").arg(id, file));
    }

    Q_INVOKABLE void setMediaPlaceholder(const QString &id, bool placeholder)
    {
        calls.append(QStringLiteral("setMediaPlaceholder:%1:%2").arg(id).arg(placeholder));
    }

signals:
    void searchTextChanged();
    void sortModeChanged();
    void selectedMediaIdChanged();
    void cacheVersionChanged();
    void generatingChanged();

private:
    QString _searchText;
    int _sortMode = 0;
    QString _selectedMediaId;
    int _cacheVersion = 0;
    bool _generating = false;
};