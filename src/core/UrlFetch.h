// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The engine-free URL fetch seam. The catalog reader needs the body in memory;
// an archive source needs the bytes on disk and a hard size bound. Both use
// libcurl directly so no external `curl` binary or PATH assumption exists, and
// both restrict the protocol set to http/https/file.

#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace genesis::core {

// Fetches `url` into memory. Follows redirects, fails on a non-2xx response,
// and caps the body at 16 MiB. On failure returns nullopt and fills `error`.
std::optional<std::string> fetch_url(const std::string &url, std::string &error);

// Downloads `url` to `path`, streaming to disk so an archive never has to be
// held in memory. Fails (and removes the partial file) on a non-2xx response,
// a write failure, or a body over `max_bytes`. The parent directory must exist.
bool download_file(const std::string &url, const std::filesystem::path &path, std::size_t max_bytes,
                   std::string &error);

} // namespace genesis::core
