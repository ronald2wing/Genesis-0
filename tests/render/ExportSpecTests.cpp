// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <string>
#include <vector>

#include "render/ExportSpec.h"

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const std::string &what)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

using namespace genesis::render;

// A spec that validates clean: the smallest well-formed export.
ExportSpec sane_spec()
{
    ExportSpec spec;
    spec.output = "out.webm";
    spec.width = 1920;
    spec.height = 1080;
    spec.clips.push_back(ExportClip::blank(ClipKind::Video, genesis::core::Rational::ZERO,
                                           genesis::core::Rational::ONE, 0));
    return spec;
}

bool contains(const std::vector<std::string> &problems, const std::string &fragment)
{
    for (const std::string &problem : problems) {
        if (problem.find(fragment) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void test_sane_spec_validates_empty()
{
    check(validate(sane_spec()).empty(), "a sane spec has no problems");
}

void test_default_spec_reports_every_problem()
{
    const std::vector<std::string> problems = validate(ExportSpec{ });
    check(contains(problems, "output"), "default spec: no output path");
    check(contains(problems, "width"), "default spec: zero width");
    check(contains(problems, "height"), "default spec: zero height");
    check(contains(problems, "clips"), "default spec: empty timeline");
}

void test_empty_output_is_rejected()
{
    ExportSpec spec = sane_spec();
    spec.output.clear();
    const std::vector<std::string> problems = validate(spec);
    check(problems.size() == 1 && contains(problems, "output"), "empty output is the only problem");
}

void test_zero_width_is_rejected()
{
    ExportSpec spec = sane_spec();
    spec.width = 0;
    const std::vector<std::string> problems = validate(spec);
    check(problems.size() == 1 && contains(problems, "width"), "zero width is the only problem");
}

void test_zero_height_is_rejected()
{
    ExportSpec spec = sane_spec();
    spec.height = 0;
    const std::vector<std::string> problems = validate(spec);
    check(problems.size() == 1 && contains(problems, "height"), "zero height is the only problem");
}

void test_non_positive_rate_is_rejected()
{
    ExportSpec spec = sane_spec();
    spec.rate_num = 0;
    check(contains(validate(spec), "rate"), "zero numerator is rejected");
    spec.rate_num = 30;
    spec.rate_den = -1;
    check(contains(validate(spec), "rate"), "negative denominator is rejected");
}

void test_empty_timeline_is_rejected()
{
    ExportSpec spec = sane_spec();
    spec.clips.clear();
    const std::vector<std::string> problems = validate(spec);
    check(problems.size() == 1 && contains(problems, "clips"),
          "empty timeline is the only problem");
}

void test_a_codec_with_no_element_is_not_a_validation_problem()
{
    // The host cannot see the engine's registry (R5); an unknown codec family
    // is a runtime refusal, so a spec naming one must still validate as a
    // well-formed host request.
    ExportSpec spec = sane_spec();
    spec.codec = static_cast<ExportCodec>(99);
    check(validate(spec).empty(), "a spec with an uninstalled codec still validates host-side");
}

void test_hevc_is_a_valid_codec_family()
{
    ExportSpec spec = sane_spec();
    spec.codec = ExportCodec::Hevc;
    check(validate(spec).empty(), "a Hevc spec validates host-side");
}

void test_ten_bit_defaults_off_and_validates()
{
    const ExportSpec spec = sane_spec();
    check(!spec.ten_bit, "a spec defaults to eight bits");
    check(!spec.hdr, "a spec defaults to SDR");

    // Ten-bit is a request the engine refuses at export time when no encoder
    // answers it, not a host validation problem (R5).
    ExportSpec ten_bit = sane_spec();
    ten_bit.ten_bit = true;
    ten_bit.codec = ExportCodec::Hevc;
    check(validate(ten_bit).empty(), "a ten-bit Hevc spec validates host-side");
}

} // namespace

int main()
{
    test_sane_spec_validates_empty();
    test_default_spec_reports_every_problem();
    test_empty_output_is_rejected();
    test_zero_width_is_rejected();
    test_zero_height_is_rejected();
    test_non_positive_rate_is_rejected();
    test_empty_timeline_is_rejected();
    test_a_codec_with_no_element_is_not_a_validation_problem();
    test_hevc_is_a_valid_codec_family();
    test_ten_bit_defaults_off_and_validates();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
