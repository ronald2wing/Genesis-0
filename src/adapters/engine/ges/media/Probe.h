// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <filesystem>
#include <optional>

namespace genesis::project {
struct NewMedia;
}

namespace genesis::adapters::engine::ges {

// Probes a media file through GStreamer's discoverer and fills the host's
// own description of it. On one side sits GstDiscoverer and the engine's
// type system, on the other the host's NewMedia; no engine type crosses
// (R5/R6). Returns nullopt when the file cannot be discovered.
std::optional<genesis::project::NewMedia> probe(const std::filesystem::path &path);

} // namespace genesis::adapters::engine::ges
