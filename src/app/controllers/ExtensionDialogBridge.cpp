// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/controllers/ExtensionDialogBridge.h"

ExtensionDialogBridge::ExtensionDialogBridge(QObject *parent) : QObject(parent) { }

void ExtensionDialogBridge::close()
{
    emit closeRequested();
}
