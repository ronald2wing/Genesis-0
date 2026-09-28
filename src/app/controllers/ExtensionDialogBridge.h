// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The bridge a contributed dialog receives to dismiss itself. The shell exposes
// this object as the `dialogBridge` context property, so a dialog's QML calls
// dialogBridge.close() rather than reaching into the shell's window. The
// overlay host connects closeRequested to tearing the overlay down, which
// deactivates the Loader and destroys the loaded component (a closed dialog
// leaks nothing). It is deliberately one method wide: the dialog's only
// lifecycle hook into the host.

#pragma once

#include <QObject>

class ExtensionDialogBridge : public QObject
{
    Q_OBJECT

public:
    explicit ExtensionDialogBridge(QObject *parent = nullptr);

public slots:
    // The contributed dialog's dismiss affordance. Raises closeRequested; the
    // overlay host clears the dialog URL, which deactivates the Loader and
    // destroys the loaded component.
    void close();

signals:
    void closeRequested();
};
