// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "render/LutLoader.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace genesis::render {

namespace {

// The ceiling on a table's texel count `Lut::from_rgb` enforces; the reader
// checks the same so it refuses a size whose cube cannot become a table
// before it ever hands the triples over.
constexpr std::uint64_t MAX_TEXELS = 1ULL << 40;

// Case-insensitive keyword comparison, so `lut_3d_size` and `LUT_3D_SIZE` are
// the same directive - real exporters do not agree on casing.
bool iequals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
        };
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

// A non-negative whole number that fits a u32, or false for anything else.
bool parse_u32(std::string_view token, std::uint32_t &out)
{
    if (token.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char c : token) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = (value * 10) + static_cast<std::uint64_t>(c - '0');
        if (value > 0xffffffffULL) {
            return false;
        }
    }
    out = static_cast<std::uint32_t>(value);
    return true;
}

// One finite float occupying the whole token, or false. `strtof` reads
// leading sign, digits and an exponent; the whole-token check rejects a
// trailing non-number, and `isfinite` rejects NaN and infinity alike.
bool parse_float(std::string_view token, float &out)
{
    if (token.empty()) {
        return false;
    }
    std::string buffer(token);
    char *end = nullptr;
    errno = 0;
    const float value = std::strtof(buffer.c_str(), &end);
    if (end != buffer.c_str() + buffer.size() || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

// The words of a line, split on ASCII whitespace.
std::vector<std::string_view> words(std::string_view line)
{
    std::vector<std::string_view> out;
    std::size_t at = 0;
    while (at < line.size()) {
        while (at < line.size()
               && (line[at] == ' ' || line[at] == '\t' || line[at] == '\r' || line[at] == '\n')) {
            ++at;
        }
        const std::size_t start = at;
        while (at < line.size() && line[at] != ' ' && line[at] != '\t' && line[at] != '\r'
               && line[at] != '\n') {
            ++at;
        }
        if (at > start) {
            out.push_back(line.substr(start, at - start));
        }
    }
    return out;
}

// Everything a file is while it is being read: the side length once seen, the
// triples in file order, and the first thing that went wrong.
struct Read
{
    std::uint32_t size = 0;
    bool size_seen = false;
    std::vector<float> rgb;
    std::string problem;
};

// Records the first problem, keeping the reason a human reads even as the
// parser walks on to find nothing more.
void fail(Read &read, std::string message)
{
    if (read.problem.empty()) {
        read.problem = std::move(message);
    }
}

// Reads one line's worth of the cube, returning early on the first problem.
// Header keywords and data rows are both handled here, so a row before the
// size line still lands in `rgb` and is checked against the size at the end.
void parse_line(std::string_view line, Read &read)
{
    const std::vector<std::string_view> ws = words(line);
    if (ws.empty() || ws[0].front() == '#') {
        return;
    }

    if (iequals(ws[0], "TITLE")) {
        return; // metadata; the text that follows is not part of the table
    }
    if (iequals(ws[0], "LUT_3D_SIZE")) {
        if (read.size_seen) {
            fail(read, "LUT_3D_SIZE appears twice");
            return;
        }
        if (ws.size() != 2 || !parse_u32(ws[1], read.size)) {
            fail(read, "LUT_3D_SIZE needs one whole number");
            return;
        }
        if (read.size < 2) {
            fail(read,
                 "LUT_3D_SIZE " + std::to_string(read.size)
                         + " is below 2, so the table cannot interpolate");
            return;
        }
        read.size_seen = true;
        return;
    }
    if (iequals(ws[0], "LUT_1D_SIZE")) {
        fail(read, "LUT_1D_SIZE is a 1D table, not the 3D LUT this reader loads");
        return;
    }
    if (iequals(ws[0], "DOMAIN_MIN") || iequals(ws[0], "DOMAIN_MAX")) {
        const bool is_min = iequals(ws[0], "DOMAIN_MIN");
        if (ws.size() != 4) {
            fail(read, std::string(ws[0]) + " needs three numbers");
            return;
        }
        float values[3];
        for (std::size_t i = 0; i < 3; ++i) {
            if (!parse_float(ws[i + 1], values[i])) {
                fail(read, std::string(ws[0]) + " holds a non-numeric value");
                return;
            }
        }
        // A .cube's data domain is the input range its rows span. The core Lut
        // samples over 0..=1 and carries no domain, so a table drawn for a
        // different range cannot be applied faithfully; the default (0 0 0 ..
        // 1 1 1) is accepted and the rest refused rather than silently
        // mis-mapped.
        const bool is_default = is_min
                ? (values[0] == 0.0F && values[1] == 0.0F && values[2] == 0.0F)
                : (values[0] == 1.0F && values[1] == 1.0F && values[2] == 1.0F);
        if (!is_default) {
            fail(read,
                 std::string(ws[0]) + " is not the default domain (" + (is_min ? "0 0 0" : "1 1 1")
                         + "); a non-default domain is refused rather than mapped");
        }
        return;
    }

    // A data row: exactly three numbers, red green blue in file order.
    if (ws.size() != 3) {
        fail(read, "a data row must be three numbers (saw " + std::to_string(ws.size()) + ")");
        return;
    }
    for (std::size_t i = 0; i < 3; ++i) {
        float value = 0.0F;
        if (!parse_float(ws[i], value)) {
            fail(read, "a data value is not a finite number: `" + std::string(ws[i]) + "`");
            return;
        }
        read.rgb.push_back(value);
    }
}

// The process-wide caches: one mutex guarding both. A table file is keyed by
// its path plus last-write time and size, so it is re-read only when it
// changes on disk; a listing is keyed by its root plus the root's last-write
// time. Both are per-process and never cleared, matching the resolve contract
// that shipped assets do not change under a running editor.
std::mutex &cache_mutex()
{
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<std::string, LutResult> &table_cache()
{
    static std::unordered_map<std::string, LutResult> cache;
    return cache;
}

std::unordered_map<std::string, std::vector<std::filesystem::path>> &listing_cache()
{
    static std::unordered_map<std::string, std::vector<std::filesystem::path>> cache;
    return cache;
}

// The cache key for a file: path, last-write time and size. A stat that fails
// means the file cannot be keyed (missing or unreadable), so the caller does
// not cache it and falls back to a plain load.
std::string file_key(const std::filesystem::path &path, std::error_code &ec)
{
    const auto ftime = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return { };
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return { };
    }
    return path.string() + "\n" + std::to_string(ftime.time_since_epoch().count()) + "\n"
            + std::to_string(size);
}

// The cache key for a directory listing: root plus its last-write time. A stat
// that fails leaves the key empty, so the caller does not cache the (empty)
// result.
std::string listing_key(const std::filesystem::path &root, std::error_code &ec)
{
    const auto ftime = std::filesystem::last_write_time(root, ec);
    if (ec) {
        return { };
    }
    return root.string() + "\n" + std::to_string(ftime.time_since_epoch().count());
}

} // namespace

LutResult load_lut_uncached(const std::filesystem::path &path)
{
    LutResult result;

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        result.problem = "cannot open " + path.string();
        return result;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    Read read;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::size_t stop = end == std::string::npos ? text.size() : end;
        parse_line(std::string_view(text).substr(start, stop - start), read);
        if (!read.problem.empty()) {
            break;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    if (read.problem.empty()) {
        if (!read.size_seen) {
            read.problem = "no LUT_3D_SIZE line";
        } else {
            // size^3 texels, checked so a huge size cannot wrap before the row
            // count is compared against it.
            std::uint64_t texels = 1;
            for (int i = 0; i < 3; ++i) {
                texels *= read.size;
                if (texels > MAX_TEXELS) {
                    read.problem = "LUT_3D_SIZE " + std::to_string(read.size)
                            + " makes a table too large to hold";
                    break;
                }
            }
            if (read.problem.empty() && read.rgb.size() != texels * 3) {
                read.problem = "LUT_3D_SIZE " + std::to_string(read.size) + " needs "
                        + std::to_string(texels) + " rows, but the file lists "
                        + std::to_string(read.rgb.size() / 3);
            }
        }
    }

    if (!read.problem.empty()) {
        result.problem = path.string() + ": " + read.problem;
        return result;
    }

    std::optional<genesis::core::Lut> lut = genesis::core::Lut::from_rgb(read.size, read.rgb);
    if (!lut) {
        result.problem = path.string() + ": the table was refused";
        return result;
    }
    result.lut = std::move(lut);
    return result;
}

LutResult load_lut(const std::filesystem::path &path)
{
    std::error_code ec;
    const std::string key = file_key(path, ec);
    if (ec) {
        // The file cannot be stat-ed (missing or unreadable): never cached, so
        // a transient missing file is retried and a real one reports the open
        // failure each time.
        return load_lut_uncached(path);
    }
    {
        std::lock_guard<std::mutex> lock(cache_mutex());
        const auto it = table_cache().find(key);
        if (it != table_cache().end()) {
            return it->second;
        }
    }
    // A failure to parse is cached too (it stat-ed fine and will not have
    // changed), so a broken shipped table is refused once, not per frame.
    LutResult result = load_lut_uncached(path);
    std::lock_guard<std::mutex> lock(cache_mutex());
    table_cache().emplace(key, result);
    return result;
}

std::vector<std::filesystem::path> available_luts_uncached(const std::filesystem::path &root)
{
    std::vector<std::filesystem::path> paths;
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec) || ec) {
        return paths;
    }
    std::filesystem::recursive_directory_iterator it(root, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && it != end) {
        if (it->is_regular_file(ec) && !ec && it->path().extension() == ".cube") {
            paths.push_back(it->path());
        }
        it.increment(ec);
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

std::vector<std::filesystem::path> available_luts(const std::filesystem::path &root)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec) || ec) {
        return { };
    }
    const std::string key = listing_key(root, ec);
    if (ec) {
        return available_luts_uncached(root);
    }
    {
        std::lock_guard<std::mutex> lock(cache_mutex());
        const auto it = listing_cache().find(key);
        if (it != listing_cache().end()) {
            return it->second;
        }
    }
    std::vector<std::filesystem::path> paths = available_luts_uncached(root);
    std::lock_guard<std::mutex> lock(cache_mutex());
    listing_cache().emplace(key, paths);
    return paths;
}

} // namespace genesis::render
