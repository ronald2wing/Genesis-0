// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace genesis::adapters::engine::ges {

// A request to make a low-resolution stand-in for a source file, used while
// editing so the monitor does not decode the original. The engine does the
// encoding (R5); the host only names the file and the size it wants.
struct ProxyRequest
{
    std::filesystem::path source;
    std::filesystem::path destination;
    std::uint32_t height = 480; // target height; width follows the aspect ratio
};

// Writes the proxy and returns its path, or nullopt on failure. Synchronous:
// callers that care about the UI must run this off the GUI thread.
std::optional<std::filesystem::path> make_proxy(const ProxyRequest &request);

// The proxy for `source` under `cache_root`, generated if it is absent or
// stale. Returns the proxy's path when a fresh one exists, and nullopt when
// there is nothing to proxy (an empty source or root, a missing source, or a
// file that cannot be transcoded). Blocking, like `make_proxy`: keep it off
// the UI thread.
std::optional<std::filesystem::path> ensure_proxy(const std::filesystem::path &source,
                                                  const std::filesystem::path &cache_root);

} // namespace genesis::adapters::engine::ges
