// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "tools/dev/json.h"

#include <cstdio>

namespace genesis::dev {

std::string json_escape(std::string_view in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (const char c : in) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[7];
                std::snprintf(buf, sizeof buf, "\\u%04x",
                              static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

void Json::pre_value()
{
    if (frames_.empty()) {
        return;
    }
    Frame &frame = frames_.back();
    // A value following a key already had its separating comma emitted by
    // key(); an array element (or the first value) does not.
    if (frame.count > 0 && !pending_key_) {
        out_ += ',';
    }
    if (pending_key_) {
        out_ += ':';
        pending_key_ = false;
    }
}

void Json::open(char kind)
{
    pre_value();
    out_ += (kind == 'o') ? '{' : '[';
    frames_.push_back({ kind, 0 });
}

void Json::close()
{
    const Frame frame = frames_.back();
    frames_.pop_back();
    out_ += (frame.kind == 'o') ? '}' : ']';
    // A closed container is itself one value in its parent.
    if (!frames_.empty()) {
        ++frames_.back().count;
    }
}

void Json::begin_object()
{
    open('o');
}

void Json::end_object()
{
    close();
}

void Json::begin_array()
{
    open('a');
}

void Json::end_array()
{
    close();
}

void Json::key(std::string_view k)
{
    Frame &frame = frames_.back();
    if (frame.count > 0) {
        out_ += ',';
    }
    out_ += '"';
    out_ += json_escape(k);
    out_ += '"';
    pending_key_ = true;
}

void Json::string(std::string_view v)
{
    pre_value();
    out_ += '"';
    out_ += json_escape(v);
    out_ += '"';
    ++frames_.back().count;
}

void Json::integer(long long v)
{
    pre_value();
    out_ += std::to_string(v);
    ++frames_.back().count;
}

void Json::boolean(bool v)
{
    pre_value();
    out_ += v ? "true" : "false";
    ++frames_.back().count;
}

void Json::null()
{
    pre_value();
    out_ += "null";
    ++frames_.back().count;
}

std::string Json::take()
{
    return std::move(out_);
}

} // namespace genesis::dev
