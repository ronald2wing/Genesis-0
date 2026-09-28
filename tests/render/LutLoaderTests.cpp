// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "effects/PackLoader.h"
#include "effects/PackSource.h"
#include "effects/Resolve.h"
#include "project/model/Effects.h"
#include "render/LutLoader.h"

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

using genesis::core::Lut;
using genesis::project::AppliedFilter;
namespace render = genesis::render;

bool near(float a, float b)
{
    return std::fabs(a - b) < 1e-6f;
}

// The assets/luts/ directory: `cwd/assets/luts`, or the nearest ancestor's, so
// the test runs from the repo root or a build directory alike.
std::optional<std::filesystem::path> locate_assets()
{
    std::filesystem::path dir = std::filesystem::current_path();
    while (true) {
        const std::filesystem::path candidate = dir / "assets" / "luts";
        std::error_code ec;
        if (std::filesystem::is_directory(candidate, ec) && !ec) {
            return candidate;
        }
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir) {
            return std::nullopt;
        }
        dir = parent;
    }
}

// The builtin genesis.packs packs/ directory, by the same walk-up rule
// PackSourceTests uses.
std::optional<std::filesystem::path> locate_packs()
{
    std::filesystem::path dir = std::filesystem::current_path();
    while (true) {
        const std::filesystem::path candidate =
                dir / "extensions" / "builtin" / "genesis.packs" / "packs";
        std::error_code ec;
        if (std::filesystem::is_directory(candidate, ec) && !ec) {
            for (const auto &entry : std::filesystem::directory_iterator(candidate, ec)) {
                if (entry.is_directory(ec)
                    && std::filesystem::exists(entry.path() / "effect.toml")) {
                    return candidate;
                }
            }
        }
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir) {
            return std::nullopt;
        }
        dir = parent;
    }
}

// One fixture written to a scratch directory, and the scratch directory itself
// removed when the test ends.
struct Fixture
{
    std::filesystem::path dir;

    explicit Fixture(const std::string &tag)
    {
        dir = std::filesystem::temp_directory_path() / tag;
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
    }

    std::filesystem::path write(const std::string &name, const std::string &contents)
    {
        const std::filesystem::path path = dir / name;
        std::ofstream out(path, std::ios::binary);
        out << contents;
        return path;
    }

    ~Fixture()
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

// A size-2 cube whose texel (r, g, b) maps to (0.25 + 0.5 r, ...), red fastest,
// so every corner is an exactly representable value and sample() must return
// those values verbatim.
constexpr std::string_view kTwoByTwo = R"(# a known table
LUT_3D_SIZE 2
0.25 0.25 0.25
0.75 0.25 0.25
0.25 0.75 0.25
0.75 0.75 0.25
0.25 0.25 0.75
0.75 0.25 0.75
0.25 0.75 0.75
0.75 0.75 0.75
)";

// A 17^3 identity table: input == output, generated so a larger table's size
// and trilinear interpolation can be checked against the known mapping.
std::string seventeen_cube()
{
    std::ostringstream out;
    out << "LUT_3D_SIZE 17\n";
    out << std::fixed << std::setprecision(6);
    const float step = 1.0f / 16.0f;
    for (int b = 0; b < 17; ++b) {
        for (int g = 0; g < 17; ++g) {
            for (int r = 0; r < 17; ++r) {
                out << static_cast<float>(r) * step << ' ' << static_cast<float>(g) * step << ' '
                    << static_cast<float>(b) * step << '\n';
            }
        }
    }
    return out.str();
}

void test_a_known_cube_parses_and_round_trips()
{
    Fixture fix("genesis-lut-known");
    const std::filesystem::path path = fix.write("known.cube", std::string(kTwoByTwo));

    const render::LutResult loaded = render::load_lut(path);
    check(loaded.lut.has_value() && loaded.problem.empty(), "the 2x2x2 cube loads");
    if (!loaded.lut) {
        std::printf("  (problem: %s)\n", loaded.problem.c_str());
        return;
    }
    check(loaded.lut->size == 2, "the size is 2");
    check(loaded.lut->rgba != nullptr && loaded.lut->rgba->size() == 8 * 4,
          "the table holds size^3 * 4 floats");

    const auto out = [&](std::array<float, 3> rgb) { return loaded.lut->sample(rgb); };
    check(near(out({ 0.0f, 0.0f, 0.0f })[0], 0.25f) && near(out({ 0.0f, 0.0f, 0.0f })[1], 0.25f)
                  && near(out({ 0.0f, 0.0f, 0.0f })[2], 0.25f),
          "sample({0,0,0}) returns the black corner's mapped value");
    check(near(out({ 1.0f, 0.0f, 1.0f })[0], 0.75f) && near(out({ 1.0f, 0.0f, 1.0f })[1], 0.25f)
                  && near(out({ 1.0f, 0.0f, 1.0f })[2], 0.75f),
          "sample({1,0,1}) returns the red+blue corner's mapped value");
    check(near(out({ 0.0f, 1.0f, 1.0f })[0], 0.25f) && near(out({ 0.0f, 1.0f, 1.0f })[1], 0.75f)
                  && near(out({ 0.0f, 1.0f, 1.0f })[2], 0.75f),
          "sample({0,1,1}) returns the green+blue corner's mapped value");
}

void test_a_17_cube_identity_round_trips()
{
    Fixture fix("genesis-lut-17");
    const std::filesystem::path path = fix.write("id.cube", seventeen_cube());

    const render::LutResult loaded = render::load_lut(path);
    check(loaded.lut.has_value() && loaded.problem.empty(), "the 17^3 identity cube loads");
    if (!loaded.lut) {
        return;
    }
    check(loaded.lut->size == 17, "the size is 17");
    const auto out = loaded.lut->sample({ 0.3f, 0.6f, 0.9f });
    check(near(out[0], 0.3f) && near(out[1], 0.6f) && near(out[2], 0.9f),
          "the identity maps {0.3, 0.6, 0.9} to itself");
}

void test_malformed_files_are_refused_with_a_reason()
{
    Fixture fix("genesis-lut-bad");

    const std::filesystem::path no_header = fix.write("no_header.cube", "1 0 0\n0 1 0\n");
    const render::LutResult a = render::load_lut(no_header);
    check(!a.lut.has_value() && !a.problem.empty(), "a file with no LUT_3D_SIZE is refused");
    check(a.problem.find("no LUT_3D_SIZE") != std::string::npos,
          "and it says no LUT_3D_SIZE (" + a.problem + ")");

    const std::filesystem::path bad_header = fix.write("bad_header.cube", "LUT_3D_SIZE\n");
    const render::LutResult b = render::load_lut(bad_header);
    check(!b.lut.has_value() && b.problem.find("whole number") != std::string::npos,
          "LUT_3D_SIZE without a number is refused (" + b.problem + ")");

    const std::filesystem::path tiny = fix.write("tiny.cube", "LUT_3D_SIZE 1\n1 0 0\n");
    const render::LutResult c = render::load_lut(tiny);
    check(!c.lut.has_value() && c.problem.find("below 2") != std::string::npos,
          "a size below 2 is refused (" + c.problem + ")");

    const std::filesystem::path wrong_rows =
            fix.write("wrong_rows.cube", "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n");
    const render::LutResult d = render::load_lut(wrong_rows);
    check(!d.lut.has_value() && d.problem.find("rows") != std::string::npos,
          "a short table is refused (" + d.problem + ")");

    const std::filesystem::path nan_value =
            fix.write("nan.cube",
                      "LUT_3D_SIZE 2\n0 0 0\nnan 0 0\n0 1 0\n1 1 0\n0 "
                      "0 1\n1 0 1\n0 1 1\n1 1 1\n");
    const render::LutResult e = render::load_lut(nan_value);
    check(!e.lut.has_value() && e.problem.find("finite") != std::string::npos,
          "a NaN value is refused (" + e.problem + ")");

    const std::filesystem::path bad_width = fix.write("bad_width.cube",
                                                      "LUT_3D_SIZE 2\n0 0\n0 0 0\n0 1 0\n1 1 "
                                                      "0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n");
    const render::LutResult f = render::load_lut(bad_width);
    check(!f.lut.has_value() && f.problem.find("three numbers") != std::string::npos,
          "a two-number row is refused (" + f.problem + ")");
}

void test_domain_handling()
{
    Fixture fix("genesis-lut-domain");

    const std::filesystem::path default_domain =
            fix.write("default.cube",
                      "LUT_3D_SIZE 2\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n"
                      "0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n");
    const render::LutResult a = render::load_lut(default_domain);
    check(a.lut.has_value() && a.problem.empty(), "the default domain (0..1) is accepted");

    const std::filesystem::path shifted =
            fix.write("shifted.cube",
                      "LUT_3D_SIZE 2\nDOMAIN_MIN 0.1 0.1 0.1\nDOMAIN_MAX 1 1 1\n"
                      "0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n");
    const render::LutResult b = render::load_lut(shifted);
    check(!b.lut.has_value() && !b.problem.empty(), "a non-default DOMAIN_MIN is refused");
    check(b.problem.find("domain") != std::string::npos,
          "and the reason names the domain (" + b.problem + ")");

    const std::filesystem::path shifted_max =
            fix.write("shifted_max.cube",
                      "LUT_3D_SIZE 2\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 2 2 2\n"
                      "0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n");
    const render::LutResult c = render::load_lut(shifted_max);
    check(!c.lut.has_value() && !c.problem.empty(), "a non-default DOMAIN_MAX is refused");
}

void test_one_real_shipped_cube_loads_and_samples()
{
    const std::optional<std::filesystem::path> root = locate_assets();
    check(root.has_value(), "assets/luts/ is found by walking up from cwd");
    if (!root) {
        return;
    }
    const std::filesystem::path path = *root / "teal_orange.cube";
    const render::LutResult loaded = render::load_lut(path);
    check(loaded.lut.has_value() && loaded.problem.empty(), "teal_orange.cube loads");
    if (!loaded.lut) {
        return;
    }
    check(loaded.lut->size == 17, "teal_orange is a 17^3 table");
    const auto mid = loaded.lut->sample({ 0.5f, 0.5f, 0.5f });
    bool plausible = true;
    for (float v : mid) {
        plausible = plausible && std::isfinite(v) && v >= 0.0f && v <= 1.0f;
    }
    check(plausible, "mid-grey samples to finite in-range colour");
}

void test_every_shipped_cube_loads()
{
    const std::optional<std::filesystem::path> root = locate_assets();
    check(root.has_value(), "assets/luts/ is found");
    if (!root) {
        return;
    }
    const std::vector<std::filesystem::path> luts = render::available_luts(*root);
    check(luts.size() == 8,
          "eight .cube files are shipped (saw " + std::to_string(luts.size()) + ")");
    int loaded = 0;
    for (const std::filesystem::path &path : luts) {
        const render::LutResult result = render::load_lut(path);
        check(result.lut.has_value() && result.problem.empty(), path.string() + " loads");
        if (result.lut) {
            ++loaded;
        }
    }
    check(loaded == 8, "all eight shipped tables load (" + std::to_string(loaded) + ")");
}

void test_the_lut_grade_pack_loads_and_resolves()
{
    const std::optional<std::filesystem::path> root = locate_packs();
    check(root.has_value(), "packs/ is found");
    if (!root) {
        return;
    }
    const std::filesystem::path dir = *root / "genesis.lut-grade";
    const genesis::effects::LoadResult loaded =
            genesis::effects::load_pack_file(dir / "effect.toml");
    check(loaded.problems.empty() && loaded.pack.has_value(),
          "genesis.lut-grade manifest loads clean");
    if (!loaded.pack) {
        return;
    }
    check(genesis::effects::is_expressible(*loaded.pack), "genesis.lut-grade is expressible");

    const genesis::effects::SourceResult resolved =
            genesis::effects::resolve_source(dir, AppliedFilter::create(loaded.pack->id), 0.0);
    check(resolved.pass.has_value() && resolved.problem.empty(),
          "genesis.lut-grade resolves to a pass");
    if (!resolved.pass) {
        return;
    }
    const std::string_view body = *resolved.pass->source;
    check(body.find("sampler2D") == std::string_view::npos, "the body declares no sampler2D");
    check(body.find("void main") != std::string_view::npos, "the body has a main");
    check(body.find("gl_FragColor") != std::string_view::npos, "the body writes gl_FragColor");
    check(body.find("texture2D") != std::string_view::npos, "the body samples the picture");
    check(body.find("lut") != std::string_view::npos, "the body consumes the lut parameter");
    check(resolved.pass->lut.has_value(), "the default selection binds the first sorted table");
    check(!resolved.pass->lut || resolved.pass->lut->size == 17, "and the bound table is 17^3");
}

// The resolver's real-LUT binding: an enum `lut` value is an index into the
// sorted `.cube` files, and the pass carries the table at that index. A value
// past the shipped tables leaves the pass's lut unset so the GLSL body stays
// the fallback rather than failing the resolve.
void test_the_lut_grade_resolver_binds_the_selected_table()
{
    const std::optional<std::filesystem::path> packs = locate_packs();
    const std::optional<std::filesystem::path> assets = locate_assets();
    check(packs.has_value() && assets.has_value(), "packs/ and assets/luts/ are found");
    if (!packs || !assets) {
        return;
    }
    const std::filesystem::path dir = *packs / "genesis.lut-grade";
    const genesis::effects::LoadResult loaded =
            genesis::effects::load_pack_file(dir / "effect.toml");
    if (!loaded.pack) {
        return;
    }

    // Index 4 is the fifth sorted table, teal_orange.cube; the pass must bind
    // the same contents the file loader returns for that name.
    AppliedFilter instance = AppliedFilter::create(loaded.pack->id);
    instance.params["lut"] = 4.0;
    const genesis::effects::SourceResult resolved =
            genesis::effects::resolve_source(dir, instance, 0.0);
    check(resolved.pass.has_value() && resolved.problem.empty(),
          "a lut-grade clip with lut = 4 resolves");
    if (!resolved.pass) {
        return;
    }
    check(resolved.pass->lut.has_value(), "the selected table is bound");
    if (!resolved.pass->lut) {
        return;
    }
    check(resolved.pass->lut->size == 17, "the bound table is 17^3");
    const render::LutResult expected = render::load_lut(*assets / "teal_orange.cube");
    check(expected.lut.has_value() && expected.lut->id == resolved.pass->lut->id,
          "index 4 binds teal_orange.cube");

    // A value past the shipped tables leaves the lut unset: the GLSL
    // approximation stays the fallback and the resolve still succeeds.
    AppliedFilter stray = AppliedFilter::create(loaded.pack->id);
    stray.params["lut"] = 99.0;
    const genesis::effects::SourceResult fallback =
            genesis::effects::resolve_source(dir, stray, 0.0);
    check(fallback.pass.has_value() && fallback.problem.empty(),
          "an out-of-range lut still resolves");
    check(fallback.pass.has_value() && !fallback.pass->lut.has_value(),
          "and leaves the lut unset for the GLSL fallback");
}

} // namespace

int main()
{
    test_a_known_cube_parses_and_round_trips();
    test_a_17_cube_identity_round_trips();
    test_malformed_files_are_refused_with_a_reason();
    test_domain_handling();
    test_one_real_shipped_cube_loads_and_samples();
    test_every_shipped_cube_loads();
    test_the_lut_grade_pack_loads_and_resolves();
    test_the_lut_grade_resolver_binds_the_selected_table();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
