// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// A minimal JSON document emitter for the dev tool's --json / report.json
// output. The tool is stdlib + POSIX only, so it cannot reuse nlohmann_json
// (a genesis_host dependency); the schemas here are fixed and small, so a
// hand-rolled emitter that tracks container frames and element counts for
// comma placement is enough.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace genesis::dev {

// Escapes one string for a JSON string literal (quotes, backslashes and
// control characters). Does not add the surrounding quotes.
std::string json_escape(std::string_view in);

// Builds one JSON document. The caller keeps begin/end pairs balanced; the
// builder tracks each open container's element count so commas land between
// elements (and never before the first or after the last).
class Json
{
public:
    void begin_object();
    void end_object();
    void begin_array();
    void end_array();

    // Emits "key": and records that the next value fills it.
    void key(std::string_view k);

    // Emits a JSON string, integer or boolean value into the innermost
    // container.
    void string(std::string_view v);
    void integer(long long v);
    void boolean(bool v);
    void null();

    std::string take();

private:
    struct Frame
    {
        char kind; // 'o' object, 'a' array
        int count; // values emitted so far
    };

    void open(char kind);
    void close();
    void pre_value();

    std::string out_;
    std::vector<Frame> frames_;
    bool pending_key_ = false;
};

} // namespace genesis::dev
