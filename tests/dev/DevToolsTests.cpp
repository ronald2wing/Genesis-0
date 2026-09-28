// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

// The dev-tools unit suite: the pure pieces of genesis-dev that need no process
// spawn or filesystem - the QML-message classifier, the /proc status RSS line
// parser, the CMakeLists genesis_add_test block parser (over a fixture), and
// the subcommand argument parsers.

#include <string>
#include <vector>

#include "../support/Checks.h"

#include "tools/dev/args.h"
#include "tools/dev/classify.h"
#include "tools/dev/feature_parser.h"
#include "tools/dev/json.h"
#include "tools/dev/suite_parser.h"

using genesis::dev::is_app_log;
using genesis::dev::is_qml_message;
using genesis::dev::Json;
using genesis::dev::parse_api_verbs;
using genesis::dev::parse_cli_verbs;
using genesis::dev::parse_command_verbs;
using genesis::dev::parse_features_args;
using genesis::dev::parse_smoke_args;
using genesis::dev::parse_status_kib;
using genesis::dev::parse_suites;
using genesis::dev::parse_suites_args;
using genesis::dev::parse_verify_args;
using genesis::dev::parse_where_args;
using genesis::test::check;
using genesis::test::summary;

namespace {

// ── QML-message classifier ────────────────────────────────────────────────

void test_qml_classifier()
{
    check(is_qml_message("qrc:/GenesisApp/Main.qml:345: TypeError: foo"),
          "a qrc:/ line is a QML message");
    check(is_qml_message("  file:///home/dev/KeyframesPane.qml:12: Error"),
          "leading whitespace is skipped");
    check(is_qml_message("file:///src/app/qml/Main.qml:1:1: ReferenceError"),
          "a file:/// line is a QML message");
    check(!is_qml_message("[app] screenshot saved: x.png"), "an [app] line is not a QML message");
    check(!is_qml_message("libpng warning: iCCP: known incorrect sRGB profile"),
          "an ordinary stderr line is not a QML message");
    check(!is_qml_message(""), "an empty line is not a QML message");

    check(is_app_log("[app] screenshot saved: x.png (1280x760)"), "an [app] line is app log");
    check(!is_app_log("qrc:/Main.qml:3: TypeError"), "a QML message is not app log");
}

// ── RSS line parser ───────────────────────────────────────────────────────

void test_rss_parser()
{
    check(parse_status_kib("VmRSS:\t 123456 kB", "VmRSS").value_or(-1) == 123456,
          "VmRSS parses its kB count");
    check(parse_status_kib("VmHWM:\t    2048 kB", "VmHWM").value_or(-1) == 2048,
          "VmHWM parses its kB count");
    check(!parse_status_kib("VmSize:\t 999 kB", "VmRSS").has_value(), "a different key is refused");
    check(!parse_status_kib("garbage", "VmRSS").has_value(), "a non-status line is refused");
    check(!parse_status_kib("VmRSS:\t", "VmRSS").has_value(), "a key with no number is refused");
    check(!parse_status_kib("", "VmRSS").has_value(), "an empty line is refused");
}

// ── CMake suite-block parser ──────────────────────────────────────────────
const char *kFixture = R"cmake(
# a comment that mentions genesis_add_test(NAME ignored ...) must not parse
genesis_add_test(NAME core SOURCES tests/CoreTests.cpp LIBRARIES genesis_host)
genesis_add_test(NAME project
    SOURCES tests/project/ProjectTests.cpp
    LIBRARIES genesis_host)
if(GENESIS_CAN_BUILD_QT_TARGETS)
genesis_add_test(NAME edit_controller
    SOURCES src/app/controllers/EditController.cpp tests/app/EditControllerTests.cpp
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/app/controllers
    LIBRARIES Qt6::Core genesis_host)
else()
message(WARNING "skipped")
endif()
genesis_add_test(NAME effect_params_controller
    SOURCES src/app/controllers/EffectParamsController.cpp tests/app/EffectParamsControllerTests.cpp
    LIBRARIES Qt6::Core genesis_host
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set(GENESIS_HOST_SOURCES
    src/core/Animation.cpp
    src/project/Editor.cpp)
add_library(genesis_engine_ges STATIC
    src/adapters/engine/ges/Export.cpp)
)cmake";

const genesis::dev::Suite *find_suite(const std::vector<genesis::dev::Suite> &suites,
                                      const std::string &name)
{
    for (const genesis::dev::Suite &suite : suites) {
        if (suite.name == name) {
            return &suite;
        }
    }
    return nullptr;
}

void test_suite_parser()
{
    const std::vector<genesis::dev::Suite> suites = parse_suites(kFixture);

    check(suites.size() == 4, "four registrations parse (comment ignored)");

    const genesis::dev::Suite *core = find_suite(suites, "core");
    check(core != nullptr, "core parses");
    if (core) {
        check(core->sources.size() == 1 && core->sources[0] == "tests/CoreTests.cpp",
              "core has one source");
        check(core->gate.empty(), "core is ungated");
    }

    const genesis::dev::Suite *project = find_suite(suites, "project");
    check(project != nullptr && project->sources.size() == 1
                  && project->sources[0] == "tests/project/ProjectTests.cpp",
          "a multi-line SOURCES block parses");

    const genesis::dev::Suite *edit = find_suite(suites, "edit_controller");
    check(edit != nullptr, "edit_controller parses");
    if (edit) {
        check(edit->gate == "GENESIS_CAN_BUILD_QT_TARGETS", "the Qt-gated block records its gate");
        check(edit->sources.size() == 2
                      && edit->sources[0] == "src/app/controllers/EditController.cpp",
              "the Qt suite's two sources parse in order");
        check(!edit->working_directory.has_value(), "no WORKING_DIRECTORY means none");
    }

    const genesis::dev::Suite *effect = find_suite(suites, "effect_params_controller");
    check(effect != nullptr, "effect_params_controller parses");
    if (effect) {
        check(effect->working_directory == "${CMAKE_CURRENT_SOURCE_DIR}",
              "WORKING_DIRECTORY parses");
        check(effect->gate.empty(), "a suite after endif() is ungated");
        check(effect->libraries.size() == 2 && effect->libraries[0] == "Qt6::Core",
              "LIBRARIES entries parse in order");
    }

    const auto libs = genesis::dev::library_sources(kFixture);
    bool host_has_editor = false;
    bool engine_has_export = false;
    for (const auto &[library, source] : libs) {
        if (library == "genesis_host" && source == "src/project/Editor.cpp") {
            host_has_editor = true;
        }
        if (library == "genesis_engine_ges" && source == "src/adapters/engine/ges/Export.cpp") {
            engine_has_export = true;
        }
    }
    check(host_has_editor, "GENESIS_HOST_SOURCES maps a file to genesis_host");
    check(engine_has_export, "genesis_engine_ges sources map to the adapter library");
}

// ── argument parsers ──────────────────────────────────────────────────────

void test_smoke_args()
{
    const auto defaults = parse_smoke_args({ });
    check(defaults.ok && defaults.seconds_ms == 8000 && defaults.max_rss_mb == 2048
                  && defaults.surfaces.empty(),
          "smoke defaults apply");

    const auto full = parse_smoke_args({ "--json", "--seconds", "500", "--surfaces",
                                         "shell, settings", "--max-rss-mb", "512", "--app=x" });
    check(full.ok && full.json && full.seconds_ms == 500 && full.max_rss_mb == 512
                  && full.app == "x" && full.surfaces.size() == 2 && full.surfaces[0] == "shell"
                  && full.surfaces[1] == "settings",
          "smoke flags parse (space and = forms)");

    check(!parse_smoke_args({ "--surfaces", "shell,bogus" }).ok, "an unknown surface is refused");
    check(!parse_smoke_args({ "--seconds", "-5" }).ok, "a negative --seconds is refused");
    check(!parse_smoke_args({ "--nope" }).ok, "an unknown smoke flag is refused");
    check(!parse_smoke_args({ "--app" }).ok, "a missing --app value is refused");
}

void test_verify_args()
{
    const auto defaults = parse_verify_args({ });
    check(defaults.ok && !defaults.skip_smoke && !defaults.strict && defaults.build_dir == "build",
          "verify defaults apply");

    const auto full = parse_verify_args({ "--skip-smoke", "--strict", "--build-dir=build-x" });
    check(full.ok && full.skip_smoke && full.strict && full.build_dir == "build-x",
          "verify flags parse");

    check(!parse_verify_args({ "--bogus" }).ok, "an unknown verify flag is refused");
}

void test_suites_args()
{
    const auto list = parse_suites_args({ "--json" });
    check(list.ok && list.json && list.files.empty(), "--json alone parses");

    const auto files = parse_suites_args({ "src/a.cpp", "tests/b.cpp", "--json" });
    check(files.ok && files.json && files.files.size() == 2 && files.files[0] == "src/a.cpp",
          "positional files parse with --json");

    check(!parse_suites_args({ "--bogus" }).ok, "an unknown suites flag is refused");
}

// ── JSON emitter ──────────────────────────────────────────────────────────

void test_json()
{
    Json json;
    json.begin_object();
    json.key("name");
    json.string("genesis");
    json.key("count");
    json.integer(3);
    json.key("enabled");
    json.boolean(true);
    json.key("missing");
    json.null();
    json.key("items");
    json.begin_array();
    json.string("a\"b\\c");
    json.string("\n\t");
    json.end_array();
    json.key("nested");
    json.begin_object();
    json.key("x");
    json.integer(1);
    json.end_object();
    json.end_object();

    check(json.take()
                  == R"({"name":"genesis","count":3,"enabled":true,"missing":null,"items":["a\"b\\c","\n\t"],"nested":{"x":1}})",
          "the JSON emitter places commas and escapes strings");

    Json rows;
    rows.begin_array();
    for (int i = 1; i <= 3; ++i) {
        rows.begin_object();
        rows.key("id");
        rows.integer(i);
        rows.end_object();
    }
    rows.end_array();
    check(rows.take() == R"([{"id":1},{"id":2},{"id":3}])",
          "an array of objects separates each element with a comma");
}

// ── feature parsers ───────────────────────────────────────────────────────

// A miniature Requests.cpp: the `return "…"` scan must pick up the dotted
// method names and skip a non-method string.
const char *kApiFixture = R"cpp(
std::string_view method_name(const Request &request)
{
    return std::visit(
            [](const auto &alternative) -> std::string_view {
                if constexpr (std::is_same_v<T, Version>) {
                    return "version";
                } else if constexpr (std::is_same_v<T, ProjectCreate>) {
                    return "project.create";
                } else if constexpr (std::is_same_v<T, EditApply>) {
                    return "edit.apply";
                }
            },
            request);
}
)cpp";

void test_api_verb_parser()
{
    const std::vector<genesis::dev::Feature> verbs = parse_api_verbs(kApiFixture);
    check(verbs.size() == 3, "three api verbs parse");
    if (verbs.size() == 3) {
        check(verbs[0].name == "version" && verbs[0].kind == "api-verb",
              "the first verb is version");
        check(verbs[1].name == "project.create", "a dotted verb parses");
        check(verbs[2].name == "edit.apply", "the last verb parses");
        check(verbs[0].source == "src/api/Requests.cpp:7", "the source carries the 1-based line");
    }
}

// A miniature main.cpp: `add_subcommand("name", "desc")` and the bare form.
const char *kCliFixture = R"cpp(
auto *inspect = app.add_subcommand("inspect", "Summarize a project document.");
auto *commands = app.add_subcommand("commands", "List command verbs.");
auto *packs = app.add_subcommand("packs", "Inspect and trial effect Packs.");
)cpp";

void test_cli_verb_parser()
{
    const std::vector<genesis::dev::Feature> verbs = parse_cli_verbs(kCliFixture);
    check(verbs.size() == 3, "three cli verbs parse");
    if (verbs.size() == 3) {
        check(verbs[0].name == "inspect" && verbs[0].description == "Summarize a project document.",
              "a cli verb carries its description");
        check(verbs[1].name == "commands", "the second cli verb parses");
        check(verbs[0].source == "src/cli/main.cpp:2", "the cli source carries the line");
    }
}

// A miniature Commands.cpp: the `{ "Name", "description" }` verb table.
const char *kCommandFixture = R"cpp(
int cmd_commands(std::ostream &out)
{
    struct Verb
    {
        const char *name;
        const char *description;
    };
    const std::vector<Verb> verbs = {
        { "AddClip", "place a clip of media on a named track" },
        { "AddMedia", "import a file into the media bin" },
        { "RemoveClips", "delete clips" },
    };
    return 0;
}
)cpp";

void test_command_verb_parser()
{
    const std::vector<genesis::dev::Feature> verbs = parse_command_verbs(kCommandFixture);
    check(verbs.size() == 3, "three command verbs parse");
    if (verbs.size() == 3) {
        check(verbs[0].name == "AddClip" && verbs[0].kind == "command-verb",
              "the first command verb parses");
        check(verbs[0].description == "place a clip of media on a named track",
              "a command verb carries its description");
        check(verbs[2].name == "RemoveClips", "the last command verb parses");
    }
}

void test_features_args()
{
    const auto defaults = parse_features_args({ });
    check(defaults.ok && !defaults.json, "features defaults apply");

    const auto json = parse_features_args({ "--json" });
    check(json.ok && json.json, "features --json parses");

    check(!parse_features_args({ "--bogus" }).ok, "an unknown features flag is refused");
}

void test_where_args()
{
    const auto defaults = parse_where_args({ "export" });
    check(defaults.ok && defaults.keywords.size() == 1 && defaults.keywords[0] == "export",
          "a where keyword parses");

    const auto multi = parse_where_args({ "export", "render", "--json" });
    check(multi.ok && multi.json && multi.keywords.size() == 2,
          "multiple keywords parse with --json");

    check(!parse_where_args({ "--bogus" }).ok, "an unknown where flag is refused");
}

} // namespace

int main()
{
    test_qml_classifier();
    test_rss_parser();
    test_suite_parser();
    test_smoke_args();
    test_verify_args();
    test_suites_args();
    test_api_verb_parser();
    test_cli_verb_parser();
    test_command_verb_parser();
    test_features_args();
    test_where_args();
    test_json();

    return summary();
}
