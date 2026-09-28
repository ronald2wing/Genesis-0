// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// A bounded XML reader for the FCPXML subset the interchange importer needs.
// Parsing is delegated to pugixml (MIT; a system or vcpkg package when present,
// otherwise the pinned tag in CMakeLists.txt); this layer enforces the bounds
// pugixml does not - one root, duplicate attributes, nesting depth and the
// element budget - and materialises the small value tree the importer walks.
// Like JsonGuard.h, a hostile or corrupted file is rejected rather than
// trusted.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace genesis::core {

struct XmlNode
{
    std::string name;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<XmlNode> children;
    // Direct text content, entities decoded, trimmed of surrounding whitespace.
    std::string text;

    // The value of the named attribute, or null when absent.
    const std::string *attribute(std::string_view key) const;

    // The first child with the given element name, or null when absent.
    const XmlNode *child(std::string_view name) const;
};

// Parses `text`. Rejects anything malformed: unbalanced tags, duplicate
// attributes, an unterminated comment or declaration, or a document over the
// depth/node budget. On failure returns nullopt and fills `error` with a
// reason. No exception escapes; out-of-range input is reported, not thrown.
std::optional<XmlNode> parse_xml(std::string_view text, std::string &error);

} // namespace genesis::core