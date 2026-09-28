// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "tools/dev/coupling.h"

#include <algorithm>
#include <filesystem>

namespace genesis::dev {

std::string repo_relative(const std::string &path)
{
    std::string out = path;
    if (out.rfind("./", 0) == 0) {
        out.erase(0, 2);
    }
    std::error_code ec;
    const std::string cwd = std::filesystem::current_path(ec).string();
    if (!cwd.empty() && out.rfind(cwd + "/", 0) == 0) {
        out.erase(0, cwd.size() + 1);
    }
    return out;
}

FileCoupling couple_file(const std::string &file, const std::vector<Suite> &suites,
                         const std::vector<std::pair<std::string, std::string>> &library_map)
{
    FileCoupling coupling;
    coupling.file = file;
    const std::string relative = repo_relative(file);

    for (const Suite &suite : suites) {
        for (const std::string &source : suite.sources) {
            if (source == relative || relative.ends_with("/" + source)
                || source.ends_with("/" + relative)) {
                coupling.compiles.push_back(suite.name);
                break;
            }
        }
    }

    if (coupling.compiles.empty()) {
        for (const auto &[library, source] : library_map) {
            if (source == relative || relative.ends_with("/" + source)) {
                coupling.link_library = library;
                break;
            }
        }
        if (!coupling.link_library.empty()) {
            for (const Suite &suite : suites) {
                if (std::find(suite.libraries.begin(), suite.libraries.end(), coupling.link_library)
                    != suite.libraries.end()) {
                    coupling.link_suites.push_back(suite.name);
                }
            }
        }
    }

    return coupling;
}

} // namespace genesis::dev
