// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Paths arrive from two callers with different habits: the CLI passes a plain
// filesystem path, QML's FileDialog passes a file:// URL. Both the confinement
// check and the handler must see the same string, so normalisation lives here
// and is applied at both points.
//
// Decoding is deliberately URL-only: a Linux filename may legitimately contain
// "%20", and percent-decoding a plain path would silently rename it.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace genesis::api {

// The filesystem path `raw` names. A file:// URL is stripped and
// percent-decoded; anything else is returned unchanged. Idempotent, so a
// decoded path can be normalised again without harm.
inline std::string local_path(std::string_view raw)
{
    if (raw.find('\0') != std::string_view::npos) {
        return { };
    }
    if (raw.rfind("file://", 0) != 0) {
        return std::string(raw);
    }

    std::string text(raw.substr(7));
    std::string decoded;
    decoded.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '%' && index + 2 < text.size()) {
            const auto hex = [](char digit) -> int {
                if (digit >= '0' && digit <= '9') {
                    return digit - '0';
                }
                if (digit >= 'a' && digit <= 'f') {
                    return digit - 'a' + 10;
                }
                if (digit >= 'A' && digit <= 'F') {
                    return digit - 'A' + 10;
                }
                return -1;
            };
            const int high = hex(text[index + 1]);
            const int low = hex(text[index + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        decoded.push_back(text[index]);
    }
    // file:///C:/... keeps a leading slash that a Windows path does not have.
    if (decoded.size() > 2 && decoded[0] == '/' && decoded[2] == ':'
        && ((decoded[1] >= 'A' && decoded[1] <= 'Z') || (decoded[1] >= 'a' && decoded[1] <= 'z'))) {
        decoded.erase(0, 1);
    }
    return decoded;
}

} // namespace genesis::api
