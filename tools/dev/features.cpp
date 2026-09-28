// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The features and where commands: the repo's orientation surface. `features`
// lists every feature surface (API verbs, CLI verbs, command verbs, QML
// surfaces, controllers) parsed live from the tree, so the index cannot drift
// from the code. `where <keyword>` is the debug loop: it maps a symptom word to
// the matching features, the ctest suites that pin them, the smoke surface that
// exercises them, and the exact command to reproduce.

#include "tools/dev/commands.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "tools/dev/classify.h"
#include "tools/dev/coupling.h"
#include "tools/dev/feature_parser.h"
#include "tools/dev/json.h"
#include "tools/dev/suite_parser.h"

namespace genesis::dev {

namespace {

std::string read_file(const std::string &path)
{
    std::ifstream in(path);
    if (!in) {
        return { };
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string lower(std::string_view in)
{
    std::string out(in);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Every feature surface, parsed from the tree. The order is the reading order
// of the orientation doc: verbs first (what the product does), then the UI and
// the controllers that drive it.
std::vector<Feature> collect_features()
{
    std::vector<Feature> features;

    const std::vector<Feature> api = parse_api_verbs(read_file("src/api/Requests.cpp"));
    features.insert(features.end(), api.begin(), api.end());

    const std::vector<Feature> cli = parse_cli_verbs(read_file("src/cli/main.cpp"));
    features.insert(features.end(), cli.begin(), cli.end());

    const std::vector<Feature> commands = parse_command_verbs(read_file("src/cli/Commands.cpp"));
    features.insert(features.end(), commands.begin(), commands.end());

    const std::vector<Feature> qml = parse_qml_surfaces("src/app/qml");
    features.insert(features.end(), qml.begin(), qml.end());

    const std::vector<Feature> controllers = parse_controllers("src/app/controllers");
    features.insert(features.end(), controllers.begin(), controllers.end());

    return features;
}

// The smoke surface a feature is exercised by, or empty. A QML surface or
// controller whose stem (minus a UI suffix like "Dialog"/"Panel"/"Pane" or
// "Controller") matches a known surface name is that surface. Everything else
// has none.
std::string smoke_surface_for(const Feature &feature)
{
    std::string stem = feature.name;
    for (const std::string &suffix : { std::string("Controller"), std::string("Dialog"),
                                       std::string("Panel"), std::string("Pane") }) {
        if (stem.size() > suffix.size()
            && stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) == 0) {
            stem = stem.substr(0, stem.size() - suffix.size());
            break;
        }
    }
    const std::string candidate = lower(stem);
    for (const std::string &surface : known_surfaces()) {
        if (surface == candidate) {
            return surface;
        }
    }
    return { };
}

// A feature's source file without the ":line" suffix, so the coupling sees a
// path. A controller's header is swapped for its .cpp, because the suites
// compile the implementation, not the declaration.
std::string source_path(const Feature &feature)
{
    std::string path = feature.source;
    const std::size_t colon = path.rfind(':');
    if (colon != std::string::npos) {
        path = path.substr(0, colon);
    }
    if (feature.kind == "controller" && path.size() > 2 && path.ends_with(".h")) {
        path.replace(path.size() - 2, 2, ".cpp");
    }
    return path;
}

// The suites that compile a feature's source file directly, via the same
// coupling the `suites` command uses. A file compiled into a genesis library
// (e.g. src/api/Requests.cpp into genesis_host) is linked by nearly every
// suite, so the link fallback is deliberately NOT used here - it would list
// every suite and drown the signal. `library_for` reports the library instead.
std::vector<std::string> suites_for(const Feature &feature, const std::vector<Suite> &suites,
                                    const std::vector<std::pair<std::string, std::string>> &libs)
{
    const FileCoupling coupling = couple_file(source_path(feature), suites, libs);
    return coupling.compiles;
}

// The genesis library a feature's source file is compiled into, or empty when a
// suite compiles it directly.
std::string library_for(const Feature &feature, const std::vector<Suite> &suites,
                        const std::vector<std::pair<std::string, std::string>> &libs)
{
    const FileCoupling coupling = couple_file(source_path(feature), suites, libs);
    return coupling.compiles.empty() ? coupling.link_library : std::string{ };
}

std::string join(const std::vector<std::string> &names)
{
    std::string out;
    for (const std::string &name : names) {
        if (!out.empty()) {
            out += ", ";
        }
        out += name;
    }
    return out;
}

// The reproduction command for a feature, by kind. These are the concrete
// entry points an agent or human runs to exercise the surface.
std::string exercise_for(const Feature &feature, const std::vector<std::string> &owning_suites)
{
    if (feature.kind == "api-verb") {
        return "build/bin/genesis-cli api   # then {\"method\":\"" + feature.name + "\",...}";
    }
    if (feature.kind == "cli-verb") {
        return "build/bin/genesis-cli " + feature.name + " --help";
    }
    if (feature.kind == "command-verb") {
        return "build/bin/genesis-cli commands   # then edit.apply {\"op\":\"" + feature.name
                + "\",...}";
    }
    if (feature.kind == "qml-surface") {
        const std::string surface = smoke_surface_for(feature);
        if (!surface.empty()) {
            return "build/bin/genesis-dev smoke --surfaces " + surface;
        }
        return "build/bin/genesis-app   # open the surface in the shell";
    }
    if (feature.kind == "controller") {
        if (!owning_suites.empty()) {
            return "ctest --test-dir build -R " + owning_suites.front() + " --output-on-failure";
        }
        return "build/bin/genesis-dev suites " + source_path(feature);
    }
    return { };
}

void print_feature(const Feature &feature, const std::vector<Suite> &suites,
                   const std::vector<std::pair<std::string, std::string>> &libs)
{
    std::printf("%-22s %-13s %s\n", feature.name.c_str(), feature.kind.c_str(),
                feature.source.c_str());
    if (!feature.description.empty()) {
        std::printf("  %s\n", feature.description.c_str());
    }
    const std::vector<std::string> owning = suites_for(feature, suites, libs);
    if (!owning.empty()) {
        std::printf("  suites: %s\n", join(owning).c_str());
    } else {
        const std::string library = library_for(feature, suites, libs);
        if (!library.empty()) {
            std::printf("  library: %s (no suite compiles it directly)\n", library.c_str());
        }
    }
    const std::string surface = smoke_surface_for(feature);
    if (!surface.empty()) {
        std::printf("  smoke:  %s\n", surface.c_str());
    }
    const std::string exercise = exercise_for(feature, owning);
    if (!exercise.empty()) {
        std::printf("  run:    %s\n", exercise.c_str());
    }
}

} // namespace

int features_command(const FeaturesArgs &args)
{
    const std::vector<Feature> features = collect_features();
    if (features.empty()) {
        std::fprintf(stderr, "features: no feature surfaces found; run from the repo root\n");
        return 2;
    }

    if (args.json) {
        Json json;
        json.begin_object();
        json.key("features");
        json.begin_array();
        for (const Feature &feature : features) {
            json.begin_object();
            json.key("name");
            json.string(feature.name);
            json.key("kind");
            json.string(feature.kind);
            json.key("source");
            json.string(feature.source);
            json.key("description");
            json.string(feature.description);
            json.end_object();
        }
        json.end_array();
        json.end_object();
        std::printf("%s\n", json.take().c_str());
        return 0;
    }

    // Group by kind so the table reads as a map of the product, not a flat dump.
    const std::vector<std::string> kinds = { "api-verb", "cli-verb", "command-verb", "qml-surface",
                                             "controller" };
    std::printf("%zu feature surfaces\n", features.size());
    for (const std::string &kind : kinds) {
        std::printf("\n%s\n", kind.c_str());
        for (const Feature &feature : features) {
            if (feature.kind != kind) {
                continue;
            }
            std::printf("  %-22s %s", feature.name.c_str(), feature.source.c_str());
            if (!feature.description.empty()) {
                std::printf("  %s", feature.description.c_str());
            }
            std::printf("\n");
        }
    }
    return 0;
}

int where_command(const WhereArgs &args)
{
    if (args.keywords.empty()) {
        std::fprintf(stderr, "where: give at least one keyword\n");
        return 2;
    }

    const std::vector<Feature> features = collect_features();
    if (features.empty()) {
        std::fprintf(stderr, "where: no feature surfaces found; run from the repo root\n");
        return 2;
    }

    const std::string cmake = read_file("CMakeLists.txt");
    const std::vector<Suite> suites = parse_suites(cmake);
    const std::vector<std::pair<std::string, std::string>> libs = library_sources(cmake);

    std::vector<Feature> matches;
    for (const Feature &feature : features) {
        const std::string haystack = lower(feature.name + " " + feature.kind + " "
                                           + feature.description + " " + feature.source);
        for (const std::string &keyword : args.keywords) {
            if (haystack.find(lower(keyword)) != std::string::npos) {
                matches.push_back(feature);
                break;
            }
        }
    }

    if (args.json) {
        Json json;
        json.begin_object();
        json.key("keywords");
        json.begin_array();
        for (const std::string &keyword : args.keywords) {
            json.string(keyword);
        }
        json.end_array();
        json.key("matches");
        json.begin_array();
        for (const Feature &feature : matches) {
            json.begin_object();
            json.key("name");
            json.string(feature.name);
            json.key("kind");
            json.string(feature.kind);
            json.key("source");
            json.string(feature.source);
            json.key("suites");
            json.begin_array();
            for (const std::string &suite : suites_for(feature, suites, libs)) {
                json.string(suite);
            }
            json.end_array();
            json.key("library");
            json.string(library_for(feature, suites, libs));
            json.key("smoke");
            json.string(smoke_surface_for(feature));
            json.key("run");
            json.string(exercise_for(feature, suites_for(feature, suites, libs)));
            json.end_object();
        }
        json.end_array();
        json.end_object();
        std::printf("%s\n", json.take().c_str());
        return matches.empty() ? 1 : 0;
    }

    if (matches.empty()) {
        std::printf("no feature matches %s\n", join(args.keywords).c_str());
        return 1;
    }

    std::printf("%zu feature(s) match %s\n\n", matches.size(), join(args.keywords).c_str());
    for (const Feature &feature : matches) {
        print_feature(feature, suites, libs);
        std::printf("\n");
    }
    return 0;
}

} // namespace genesis::dev
