// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "api/TextFile.h"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

#include "core/AtomicWrite.h"
#include "core/JsonGuard.h"

namespace genesis::api {

namespace {

// Whether `text` is well-formed UTF-8 (RFC 3629): continuation bytes, overlong
// forms and surrogates are refused, so no malformed sequence reaches a JSON
// string.
bool is_valid_utf8(std::string_view text)
{
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        if (lead < 0x80) {
            ++index;
            continue;
        }
        std::size_t length = 0;
        std::uint32_t codepoint = 0;
        std::uint32_t minimum = 0;
        if ((lead & 0xE0) == 0xC0) {
            length = 2;
            codepoint = lead & 0x1F;
            minimum = 0x80;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
            codepoint = lead & 0x0F;
            minimum = 0x800;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
            codepoint = lead & 0x07;
            minimum = 0x10000;
        } else {
            return false; // a non-leading byte where a lead byte belongs
        }
        if (index + length > text.size()) {
            return false; // truncated sequence
        }
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto continuation = static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xC0) != 0x80) {
                return false; // a continuation byte does not follow
            }
            codepoint = (codepoint << 6) | (continuation & 0x3F);
        }
        if (codepoint < minimum) {
            return false; // overlong encoding
        }
        if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            return false; // beyond U+10FFFF, or a surrogate half
        }
        index += length;
    }
    return true;
}

} // namespace

bool read_text_file(const std::string &path, std::string &out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.clear();
    char buffer[65536];
    while (in) {
        in.read(buffer, sizeof(buffer));
        const std::streamsize got = in.gcount();
        if (got <= 0) {
            break;
        }
        if (out.size() + static_cast<std::size_t>(got) > core::kMaxDocumentJsonBytes) {
            return false; // refuse an oversized file before it is buffered whole
        }
        out.append(buffer, static_cast<std::size_t>(got));
    }
    if (in.bad()) {
        return false; // a read error after some bytes were produced
    }
    return is_valid_utf8(out);
}

bool write_text_atomic(const std::string &path, const std::string &text)
{
    return core::write_file_atomic(path, text);
}

} // namespace genesis::api
