// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Whether a manifest-supplied string can be used as one path segment: no
// separators and no parent traversal. Both the model store and the updater lay
// untrusted ids out under a root, so the check is shared rather than re-derived
// per store.

#pragma once

#include <string_view>

namespace genesis::core {

inline bool is_safe_path_segment(std::string_view value)
{
    return !value.empty() && value.find('/') == std::string_view::npos
            && value.find('\\') == std::string_view::npos
            && value.find("..") == std::string_view::npos;
}

} // namespace genesis::core
