#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Ported from Concat's concat-project/src/doc.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.

#include <nlohmann/json.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/ClipTimeline.h"
#include "project/Project.h"

namespace genesis::project {

// The document version this build writes and the newest it can read. A
// document from a newer build is refused, not guessed at.
inline constexpr int DOCUMENT_VERSION = 1;

// Settings a document may carry beyond the project itself (mirror doc.rs's
// DocumentSettings fields, with the same defaults). The frame and rate here
// are what a project *starts* as - the launch screen's pickers, handed to the
// first timeline of a project that has no document yet. Once there is a
// document, every timeline carries its own (Timeline::video) and these are
// not consulted again.
struct DocumentSettings
{
    // The project's display name, written as the document's `name` field.
    std::string name;
    // Output frame width in pixels.
    std::uint32_t width = 1920;
    // Output frame height in pixels.
    std::uint32_t height = 1080;
    // Numerator of the output frame rate, e.g. 30000 for 29.97fps.
    std::int64_t rate_num = 30;
    // Denominator of the output frame rate, e.g. 1001 for 29.97fps.
    std::int64_t rate_den = 1;
};

// The version field of a parsed document, if it carries one.
std::optional<int> document_version(const nlohmann::json &doc);

// Decodes a time field: a JSON number is Concat's f64 seconds and is snapped
// to the nearest 1/1,000,000 via Rational::approximate; a JSON string is the
// host's exact "num/den" spelling via Rational::parse. Anything else, or a
// non-finite number, is nullopt. One helper for every time field so the two
// spellings are handled in one place.
std::optional<Rational> decode_time(const nlohmann::json &value);

// Encodes a time the host way: the exact "num/den" string. Never a number -
// a number would be f64 seconds, which is what the host model exists to avoid.
std::string encode_time(Rational value);

// Reads a Concat or host-native document. Returns nullopt when the document is
// unusable (not an object, a version this build cannot read, or garbage) - a
// damaged entry INSIDE a readable document is dropped, not fatal.
std::optional<Project> from_document(const nlohmann::json &doc,
                                     const DocumentSettings &settings = { });

// Writes the host-native document: camelCase keys, a version field, times as
// exact "num/den" strings.
nlohmann::json to_document(const Project &project, const DocumentSettings &settings = { });

} // namespace genesis::project
