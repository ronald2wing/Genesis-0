// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "core/XmlRead.h"

#include <pugixml.hpp>

#include <cstddef>
#include <string>
#include <utility>

namespace genesis::core {

namespace {

// The deepest nesting the reader accepts. FCPXML nests resources -> spine ->
// clip -> children, a handful of levels; 256 is far beyond any real document.
constexpr int kMaxXmlDepth = 256;

// The most elements the reader will materialise. One million nodes is far above
// any real FCPXML and still bounds what a hostile file can make the importer
// hold.
constexpr std::size_t kMaxXmlElements = 1000000;

bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Removes surrounding whitespace, leaving the interior untouched. pugixml keeps
// text runs verbatim; the importer wants the trimmed edges only.
void trim_whitespace(std::string &text)
{
    std::size_t first = 0;
    while (first < text.size() && is_space(text[first])) {
        ++first;
    }
    std::size_t last = text.size();
    while (last > first && is_space(text[last - 1])) {
        --last;
    }
    text = text.substr(first, last - first);
}

// Converts pugixml's tree into the value tree the FCPXML reader walks. The
// checks pugixml itself does not make are applied here: duplicate attributes,
// the nesting bound and the element budget. Its parse result already covers
// malformed markup, including an unterminated comment or tag.
bool convert_node(const pugi::xml_node &source, XmlNode &out, int depth, std::size_t &count,
                  std::string &error)
{
    if (depth > kMaxXmlDepth) {
        error = "elements nest deeper than 256 levels";
        return false;
    }
    if (++count > kMaxXmlElements) {
        error = "the document exceeds the 1,000,000 element budget";
        return false;
    }

    out.name = source.name();
    for (const pugi::xml_attribute attribute : source.attributes()) {
        for (const auto &[name, value] : out.attributes) {
            if (name == attribute.name()) {
                error = "duplicate attribute \"" + std::string(attribute.name()) + "\" on <"
                        + out.name + ">";
                return false;
            }
        }
        out.attributes.emplace_back(attribute.name(), attribute.value());
    }

    std::string text;
    for (const pugi::xml_node child : source.children()) {
        if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) {
            // CDATA is literal text; pugixml keeps it as its own node type and
            // does not expand entities inside it.
            text += child.value();
            continue;
        }
        if (child.type() != pugi::node_element) {
            continue; // comments and processing instructions are not values
        }
        XmlNode converted;
        if (!convert_node(child, converted, depth + 1, count, error)) {
            return false;
        }
        out.children.push_back(std::move(converted));
    }

    trim_whitespace(text);
    out.text = std::move(text);
    return true;
}

} // namespace

const std::string *XmlNode::attribute(std::string_view key) const
{
    for (const auto &[name, value] : attributes) {
        if (name == key) {
            return &value;
        }
    }
    return nullptr;
}

const XmlNode *XmlNode::child(std::string_view name) const
{
    for (const XmlNode &node : children) {
        if (node.name == name) {
            return &node;
        }
    }
    return nullptr;
}

std::optional<XmlNode> parse_xml(std::string_view text, std::string &error)
{
    if (text.empty()) {
        error = "the document has no root element";
        return std::nullopt;
    }

    pugi::xml_document document;
    const pugi::xml_parse_result result = document.load_buffer(text.data(), text.size());
    if (!result) {
        error = std::string(result.description()) + " at offset "
                + std::to_string(static_cast<std::ptrdiff_t>(result.offset));
        return std::nullopt;
    }

    // pugixml accepts several top-level elements; an XML document has exactly
    // one, and the importer's `tree->child("fcpxml")` contract depends on it.
    const pugi::xml_node root = document.first_child();
    if (!root || root.type() != pugi::node_element) {
        error = "the document has no root element";
        return std::nullopt;
    }
    for (pugi::xml_node sibling = root.next_sibling(); sibling; sibling = sibling.next_sibling()) {
        if (sibling.type() == pugi::node_element) {
            error = "unexpected content after the root element";
            return std::nullopt;
        }
    }

    XmlNode converted;
    std::size_t count = 0;
    if (!convert_node(root, converted, 0, count, error)) {
        return std::nullopt;
    }

    // The wrapper node matches the previous reader's shape: the document holds
    // the root as its only child.
    XmlNode document_node;
    document_node.children.push_back(std::move(converted));
    return document_node;
}

} // namespace genesis::core
