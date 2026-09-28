// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "render/ExportSpec.h"

namespace genesis::render {

std::vector<std::string> validate(const ExportSpec &spec)
{
    std::vector<std::string> problems;
    if (spec.output.empty()) {
        problems.push_back("no output path");
    }
    if (spec.width == 0) {
        problems.push_back("frame width is zero");
    }
    if (spec.height == 0) {
        problems.push_back("frame height is zero");
    }
    if (spec.rate_num <= 0 || spec.rate_den <= 0) {
        problems.push_back("frame rate is not positive");
    }
    if (spec.clips.empty()) {
        problems.push_back("timeline has no clips");
    }
    return problems;
}

} // namespace genesis::render
