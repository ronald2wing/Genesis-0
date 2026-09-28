// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// A small string catalogue behind `i18n.translate(key)`, so the start
// screen's strings live in one place and can be translated without a Qt
// translation file. The full QTranslator/.qm path is out of scope for this
// increment; this keeps the one pane's strings routable. The method is named
// `translate`, not `tr`, so it does not shadow QObject::tr.
//
// The QML context property remains named "i18n" (set in AppShell.cpp); only
// this C++ type carries the name.

#pragma once

#include <QObject>
#include <QString>

class I18n : public QObject
{
    Q_OBJECT
public:
    explicit I18n(QObject *parent = nullptr);

    Q_INVOKABLE QString translate(const QString &key) const;
};
