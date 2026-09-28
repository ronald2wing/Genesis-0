// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

namespace genesis::project {

// Rust's `str::trim`: whitespace off both ends. ASCII whitespace, since a
// title's words arrive as bytes, not as Unicode-aware input.
inline std::string trim(std::string_view input)
{
    constexpr std::string_view k_whitespace = " \t\n\r\f\v";
    const std::size_t first = input.find_first_not_of(k_whitespace);
    if (first == std::string_view::npos) {
        return { };
    }
    const std::size_t last = input.find_last_not_of(k_whitespace);
    return std::string(input.substr(first, last - first + 1));
}

// A one-line label for a block of text: the first non-empty line, trimmed
// and cut to forty characters, "Text" when that leaves nothing.
inline std::string first_line(std::string_view content)
{
    std::string label = "Text";
    std::size_t start = 0;
    while (start <= content.size()) {
        const std::size_t end = content.find('\n', start);
        std::string_view candidate = content.substr(
                start, end == std::string_view::npos ? content.size() - start : end - start);
        if (!candidate.empty() && candidate.back() == '\r') {
            candidate.remove_suffix(1);
        }
        const std::string trimmed = trim(candidate);
        if (!trimmed.empty()) {
            label = trimmed.substr(0, 40);
            break;
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return label;
}

// "Timeline 2" from the highest number already in use, not from the count.
// Only names of the exact shape "`name` N" count: a renamed "Timeline 2 v10"
// is somebody's own name, not a number in the sequence.
template <typename Range>
std::string next_numbered(std::string_view name, const Range &existing)
{
    // `existing` is a range of entities each with a `std::string name` member.
    std::uint64_t highest = 0;
    for (const auto &entity : existing) {
        const std::string &candidate = entity.name;
        if (!candidate.starts_with(name)) {
            continue;
        }
        const std::string_view rest = std::string_view(candidate).substr(name.size());
        if (rest.empty() || rest.front() != ' ') {
            continue;
        }
        const std::string_view digits = rest.substr(1);
        std::uint64_t value = 0;
        const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (ec != std::errc{ } || ptr != digits.data() + digits.size()) {
            continue;
        }
        highest = std::max(highest, value);
    }
    std::string out(name);
    out += ' ';
    out += std::to_string(highest + 1);
    return out;
}

} // namespace genesis::project
