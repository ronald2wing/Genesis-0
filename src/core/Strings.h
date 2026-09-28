// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// ASCII string helpers shared across the host. ASCII on purpose: every caller
// trims bytes from manifests, documents, or wire lines, never Unicode-aware
// user input, and a locale-free rule keeps the behaviour identical on every
// platform.

#pragma once

#include <string_view>

namespace genesis::core {

inline constexpr std::string_view kAsciiWhitespace = " \t\n\r\f\v";

// `text` with ASCII whitespace removed from both ends. A view into the input:
// the caller owns the backing storage.
inline std::string_view trim_ascii(std::string_view text)
{
    const std::size_t first = text.find_first_not_of(kAsciiWhitespace);
    if (first == std::string_view::npos) {
        return { };
    }
    const std::size_t last = text.find_last_not_of(kAsciiWhitespace);
    return text.substr(first, last - first + 1);
}

// Empty, or ASCII whitespace only.
inline bool is_blank_ascii(std::string_view text)
{
    return text.find_first_not_of(kAsciiWhitespace) == std::string_view::npos;
}

} // namespace genesis::core
