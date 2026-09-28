// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <toml++/toml.hpp>

#include "core/TomlRead.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

using namespace genesis::core;

// --- is_id_segment --------------------------------------------------------

void test_empty_segment()
{
    check(!is_id_segment(""), "empty is not a valid id segment");
    check(!is_id_segment(std::string_view()), "default string_view is not a valid id segment");
}

void test_valid_segment()
{
    check(is_id_segment("abc"), "lowercase letters");
    check(is_id_segment("abc-123"), "lowercase letters, digits, hyphens");
    check(is_id_segment("a"), "single letter");
    check(is_id_segment("1"), "single digit");
    check(is_id_segment("a-1"), "letter hyphen digit");
    check(is_id_segment("abc-def"), "multiple segments joined by hyphens");
}

void test_invalid_uppercase()
{
    check(!is_id_segment("Abc"), "uppercase is invalid");
    check(!is_id_segment("ABC"), "all uppercase is invalid");
    check(!is_id_segment("abcDef"), "mixed case is invalid");
}

void test_invalid_special()
{
    check(!is_id_segment("abc_def"), "underscore is invalid in id segment");
    check(!is_id_segment("abc.def"), "dot is invalid in id segment");
    check(!is_id_segment("abc def"), "space is invalid");
    check(!is_id_segment("abc/def"), "slash is invalid");
    check(!is_id_segment("abc@def"), "at-sign is invalid");
}

void test_leading_digit_is_valid()
{
    check(is_id_segment("123"), "all digits is valid");
    check(is_id_segment("1abc"), "leading digit is valid");
}

// --- is_namespaced_id -----------------------------------------------------

void test_no_dot()
{
    check(!is_namespaced_id("abc"), "no dot is not a namespaced id");
    check(!is_namespaced_id("abc-123"), "no dot with hyphens is not a namespaced id");
}

void test_valid_namespaced()
{
    check(is_namespaced_id("author.name"), "author.name is valid");
    check(is_namespaced_id("a.b"), "single-letter segments are valid");
    check(is_namespaced_id("abc-123.def-456"), "segments with hyphens and digits");
}

void test_empty_segment_in_namespaced()
{
    check(!is_namespaced_id("a."), "empty second segment is invalid");
    check(!is_namespaced_id(".b"), "empty first segment is invalid");
    check(!is_namespaced_id("."), "only dot is invalid");
}

void test_first_dot_splits_only()
{
    check(!is_namespaced_id("a.b.c"), "three segments is invalid: only first dot splits");
    check(!is_namespaced_id("author.name.extra"), "triple segment is invalid");
}

// --- error messages -------------------------------------------------------

// Minimal TomlErrorSink that captures failures.
struct TestSink : TomlErrorSink
{
    std::vector<std::string> wheres;
    std::vector<std::string> messages;

    void fail(std::string where, std::string message) override
    {
        wheres.push_back(std::move(where));
        messages.push_back(std::move(message));
    }
};

void test_require_string_missing()
{
    TestSink sink;
    toml::table table;
    std::string out;
    require_string(table, "id", "extension.id", out, sink);
    check(sink.messages.size() == 1, "missing key records one problem");
    check(sink.messages[0] == "missing `id`", "missing key message is exact");
}

void test_require_string_wrong_type()
{
    TestSink sink;
    toml::table table;
    table.insert_or_assign("id", 42);
    std::string out;
    require_string(table, "id", "extension.id", out, sink);
    check(sink.messages.size() == 1, "wrong type records one problem");
    check(sink.messages[0] == "`id` must be a string", "wrong type message is exact");
}

void test_read_string_default_preserved()
{
    TestSink sink;
    toml::table table;
    std::string out = "default-value";
    read_string(table, "category", "effect.category", out, sink);
    check(sink.messages.empty(), "absent key records no problem");
    check(out == "default-value", "default is preserved when key is absent");
}

void test_read_string_wrong_type()
{
    TestSink sink;
    toml::table table;
    table.insert_or_assign("category", 42);
    std::string out;
    read_string(table, "category", "effect.category", out, sink);
    check(sink.messages.size() == 1, "wrong type records one problem");
    check(sink.messages[0] == "`category` must be a string", "wrong type message is exact");
}

void test_read_optional_string_absent()
{
    TestSink sink;
    toml::table table;
    std::optional<std::string> out;
    read_optional_string(table, "entry", "extension.entry", out, sink);
    check(sink.messages.empty(), "absent optional key records no problem");
    check(!out.has_value(), "optional stays empty when key is absent");
}

void test_read_optional_string_wrong_type()
{
    TestSink sink;
    toml::table table;
    table.insert_or_assign("entry", true);
    std::optional<std::string> out;
    read_optional_string(table, "entry", "extension.entry", out, sink);
    check(sink.messages.size() == 1, "wrong type records one problem");
    check(sink.messages[0] == "`entry` must be a string", "wrong type message is exact");
}

void test_read_uint32_wrong_type()
{
    TestSink sink;
    toml::table table;
    table.insert_or_assign("version", "not-a-number");
    std::uint32_t out = 0;
    read_uint32(table, "version", "extension.version", out, sink);
    check(sink.messages.size() == 1, "wrong type records one problem");
    check(sink.messages[0] == "`version` must be a whole number", "wrong type message is exact");
    check(out == 0, "value is unchanged on error");
}

void test_read_uint32_negative()
{
    TestSink sink;
    toml::table table;
    table.insert_or_assign("version", -1);
    std::uint32_t out = 0;
    read_uint32(table, "version", "extension.version", out, sink);
    check(sink.messages.size() == 1, "negative value records one problem");
    check(sink.messages[0] == "`version` must be a whole number", "negative message is exact");
    check(out == 0, "value is unchanged on error");
}

void test_read_uint32_valid()
{
    TestSink sink;
    toml::table table;
    table.insert_or_assign("version", 3);
    std::uint32_t out = 0;
    read_uint32(table, "version", "extension.version", out, sink);
    check(sink.messages.empty(), "valid uint32 records no problem");
    check(out == 3, "value is assigned");
}

void test_read_int32_wrong_type()
{
    TestSink sink;
    toml::table table;
    table.insert_or_assign("order", "abc");
    std::int32_t out = 0;
    read_int32(table, "order", "effect.order", out, sink);
    check(sink.messages.size() == 1, "wrong type records one problem");
    check(sink.messages[0] == "`order` must be a whole number", "wrong type message is exact");
    check(out == 0, "value is unchanged on error");
}

void test_read_double_wrong_type()
{
    TestSink sink;
    toml::table table;
    table.insert_or_assign("min", "not-a-number");
    double out = 0.0;
    read_double(table, "min", "param[0].min", out, sink);
    check(sink.messages.size() == 1, "wrong type records one problem");
    check(sink.messages[0] == "`min` must be a number", "wrong type message is exact");
}

void test_read_double_valid()
{
    TestSink sink;
    toml::table table;
    table.insert_or_assign("min", 1.5);
    double out = 0.0;
    read_double(table, "min", "param[0].min", out, sink);
    check(sink.messages.empty(), "valid double records no problem");
    check(std::abs(out - 1.5) < 1e-12, "value is assigned correctly");
}

void test_read_bool_wrong_type()
{
    TestSink sink;
    toml::table table;
    table.insert_or_assign("animate", "yes");
    bool out = false;
    read_bool(table, "animate", "param[0].animate", out, sink);
    check(sink.messages.size() == 1, "wrong type records one problem");
    check(sink.messages[0] == "`animate` must be true or false", "wrong type message is exact");
    check(out == false, "value is unchanged on error");
}

void test_read_bool_valid()
{
    TestSink sink;
    toml::table table;
    table.insert_or_assign("animate", true);
    bool out = false;
    read_bool(table, "animate", "param[0].animate", out, sink);
    check(sink.messages.empty(), "valid bool records no problem");
    check(out == true, "value is assigned");
}

void test_at_empty_position()
{
    const std::string result = at(toml::source_position{ });
    check(result.empty(), "empty source_position returns empty string");
}

} // namespace

int main()
{
    test_empty_segment();
    test_valid_segment();
    test_invalid_uppercase();
    test_invalid_special();
    test_leading_digit_is_valid();

    test_no_dot();
    test_valid_namespaced();
    test_empty_segment_in_namespaced();
    test_first_dot_splits_only();

    test_require_string_missing();
    test_require_string_wrong_type();
    test_read_string_default_preserved();
    test_read_string_wrong_type();
    test_read_optional_string_absent();
    test_read_optional_string_wrong_type();
    test_read_uint32_wrong_type();
    test_read_uint32_negative();
    test_read_uint32_valid();
    test_read_int32_wrong_type();
    test_read_double_wrong_type();
    test_read_double_valid();
    test_read_bool_wrong_type();
    test_read_bool_valid();

    test_at_empty_position();

    return genesis::test::summary();
}