// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/support/I18n.h"

#include <map>
#include <string>

I18n::I18n(QObject *parent) : QObject(parent) { }

QString I18n::translate(const QString &key) const
{
    static const std::map<std::string, std::string> catalogue = {
        { "app.title", "Genesis-0" },
        { "start.recentProjects", "Recent projects" },
        { "start.empty", "Nothing here yet — projects you open will appear." },
        { "start.open", "Open" },
        { "start.newProject", "New Project" },
        { "start.continue", "Continue to Editor" },
        { "start.templates", "Templates" },
        { "start.templatesEmpty", "No templates yet." },
        { "start.create", "Create" },
        { "start.namePlaceholder", "Untitled project" },
        { "tray.show", "Show Genesis-0" },
        { "tray.quit", "Quit" },
    };
    const auto it = catalogue.find(key.toStdString());
    if (it == catalogue.end()) {
        return key;
    }
    return QString::fromStdString(it->second);
}
