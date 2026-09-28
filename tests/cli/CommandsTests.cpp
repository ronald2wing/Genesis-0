// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gst/gst.h>

#include <nlohmann/json.hpp>

#include "api/Api.h"
#include "cli/Commands.h"
#include "cli/JsonRpc.h"
#include "effects/Pack.h"
#include "extensions/DisabledList.h"
#include "extensions/RemovedList.h"
#include "extensions/RevokedList.h"
#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/model/Media.h"
#include "project/Document.h"
#include "project/Editor.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

using namespace genesis::cli;
using namespace genesis::project;

bool contains(const std::string &haystack, const std::string &needle)
{
    return haystack.find(needle) != std::string::npos;
}

// Writes `contents` to a temp file and returns its path. The temp dir is
// created on demand; fixtures never touch the source tree.
std::string write_temp(const std::string &name, const std::string &contents)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "genesis-cli-tests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / name;
    std::ofstream out(file, std::ios::binary);
    out << contents;
    return file.string();
}

// Writes a Pack directory (an effect.toml with the given body) under the temp
// area and returns it. The `packs trial` verb reads a standalone effect.toml,
// so only the trial tests use this helper.
std::string write_pack_dir(const std::string &name, const std::string &manifest_text)
{
    const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "genesis-cli-tests" / name;
    std::filesystem::create_directories(dir);
    std::ofstream out(dir / "effect.toml");
    out << manifest_text;
    return dir.string();
}

// Writes an Extension directory (an extension.toml with the given manifest
// text) under the temp area and returns it. The install verb reads only
// extension.toml, so install/config tests use this helper.
std::string write_extension_dir(const std::string &name, const std::string &manifest_text)
{
    const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "genesis-cli-tests" / name;
    std::filesystem::create_directories(dir);
    std::ofstream out(dir / "extension.toml");
    out << manifest_text;
    return dir.string();
}

// A GLSL body that compiles against the fragment the engine composes (prelude
// with `tex`/`v_texcoord`, then this body): it declares nothing, samples the
// one texture, and writes the colour.
const std::string valid_body = "void main () { gl_FragColor = texture2D (tex, v_texcoord); }\n";

// Writes a Pack directory holding both an effect.toml (the given manifest) and
// an effect.glsl (the given body) and returns it. The trial verb takes the
// directory, so both files must be present for a shader compile to run.
std::string write_pack_dir_with_body(const std::string &name, const std::string &manifest_text,
                                     const std::string &body)
{
    const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "genesis-cli-tests" / name;
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir / "effect.toml");
        out << manifest_text;
    }
    {
        std::ofstream out(dir / "effect.glsl", std::ios::binary);
        out << body;
    }
    return dir.string();
}

// A minimal, valid manifest for the given id and version.
std::string pack_manifest(const std::string &id, int version)
{
    return "[effect]\n"
           "id = \""
            + id
            + "\"\n"
              "name = \"Blur\"\n"
              "kind = \"effect\"\n"
              "version = "
            + std::to_string(version) + "\n";
}

// A data-only native-extension manifest (no `entry`, so no library
// is loaded). `dependencies` names the extension ids this one depends on.
std::string native_manifest(const std::string &id, int version,
                            const std::vector<std::string> &dependencies = { })
{
    std::string text = "[extension]\n";
    text += "id = \"" + id + "\"\n";
    text += "name = \"Data " + id + "\"\n";
    text += "version = " + std::to_string(version) + "\n";
    if (!dependencies.empty()) {
        text += "requires = [";
        for (std::size_t i = 0; i < dependencies.size(); ++i) {
            if (i != 0) {
                text += ", ";
            }
            text += "\"" + dependencies[i] + "\"";
        }
        text += "]\n";
    }
    return text;
}

// Writes a native-extension store entry `<store>/<id>/<version>/extension.toml`.
void write_native_extension(const std::string &store, const std::string &id, int version,
                            const std::vector<std::string> &dependencies = { })
{
    const std::filesystem::path root = std::filesystem::path(store) / id / std::to_string(version);
    std::filesystem::create_directories(root);
    std::ofstream out(root / "extension.toml");
    out << native_manifest(id, version, dependencies);
}

// One probed media item, as the host would describe it.
Command media(const std::string &path, Rational duration)
{
    NewMedia item;
    item.path = path;
    item.name = path.substr(path.find_last_of('/') + 1);
    item.duration = duration;
    item.kind = MediaKind::Video;
    item.width = 1920;
    item.height = 1080;
    item.frame_rate = 30.0;
    item.frame_rate_fraction = "30/1";
    item.video_codec = "h264";
    item.has_audio = true;
    item.audio_codec = "aac";
    return Command{ AddMedia{ std::move(item) } };
}

// A document with two media items and two clips on the founding timeline:
// clip A at [0, 10), clip B at [10, 10 + 1/3), so the timeline runs 31/3 s
// across four tracks. Returns the path to the written document.
std::string make_document()
{
    Editor editor;
    const auto add_a = editor.apply(media("/footage/a.mp4", Rational{ 10, 1 }));
    const std::string a_id = *add_a->created_id;
    const auto add_b = editor.apply(media("/footage/b.mp4", Rational{ 1, 3 }));
    const std::string b_id = *add_b->created_id;
    const std::string track = editor.project().active().tracks[0].id;
    (void)editor.apply(AddClip{ a_id, track, Rational::ZERO, false });
    (void)editor.apply(AddClip{ b_id, track, Rational{ 10, 1 }, false });

    DocumentSettings settings;
    settings.name = "Test";
    return write_temp("project.json", to_document(editor.project(), settings).dump());
}

// A document with the founding timeline but no clips: the render verb refuses
// it at validation rather than building an empty export.
std::string make_empty_document()
{
    Editor editor;
    DocumentSettings settings;
    settings.name = "Empty";
    return write_temp("empty.json", to_document(editor.project(), settings).dump());
}

void test_inspect_human()
{
    const std::string doc = make_document();
    std::ostringstream out;
    check(cmd_inspect(out, doc, false) == EXIT_OK, "inspect exits 0");
    const std::string text = out.str();
    check(contains(text, "name: Test"), "inspect names the project");
    check(contains(text, "version: 1"), "inspect reports the version");
    check(contains(text, "frame: 1920x1080 @ 30/1"), "inspect reports the frame");
    check(contains(text, "media: 2"), "inspect counts the media");
    check(contains(text, "fonts: 0"), "inspect counts the fonts");
    check(contains(text, "timelines: 1"), "inspect counts the timelines");
    check(contains(text, "tracks=4"), "inspect reports the four starter lanes");
    check(contains(text, "clips=2"), "inspect counts the clips");
    check(contains(text, "duration=31/3"), "inspect reports the exact duration");
}

void test_inspect_json()
{
    const std::string doc = make_document();
    std::ostringstream out;
    check(cmd_inspect(out, doc, true) == EXIT_OK, "inspect --json exits 0");
    const nlohmann::json parsed = nlohmann::json::parse(out.str());
    check(parsed["name"] == "Test", "json names the project");
    check(parsed["version"] == 1, "json reports the version");
    check(parsed["frame"]["width"] == 1920, "json reports the width");
    check(parsed["frame"]["rateNum"] == 30, "json reports the rate");
    check(parsed["media"] == 2, "json counts the media");
    check(parsed["fonts"] == 0, "json counts the fonts");
    check(parsed["timelines"].size() == 1, "json has one timeline");
    check(parsed["timelines"][0]["tracks"] == 4, "json reports the lanes");
    check(parsed["timelines"][0]["clips"] == 2, "json counts the clips");
    check(parsed["timelines"][0]["duration"] == "31/3", "json has the exact duration");
}

void test_validate_ok()
{
    const std::string doc = make_document();
    std::ostringstream out;
    check(cmd_validate(out, doc) == EXIT_OK, "validate exits 0");
    const std::string text = out.str();
    check(contains(text, "ok "), "validate reports ok");
    check(contains(text, "2 media"), "validate counts the media");
    check(contains(text, "1 timeline(s)"), "validate counts the timelines");
    check(contains(text, "0 font(s)"), "validate counts the fonts");
}

void test_validate_refuses_garbage()
{
    const std::string garbage = write_temp("garbage.json", "not json at all");
    std::ostringstream out;
    check(cmd_validate(out, garbage) != EXIT_OK, "garbage does not validate");
    check(contains(out.str(), "error: "), "the refusal is an error");
}

void test_validate_refuses_a_newer_build()
{
    const std::string doc = make_document();
    // Bump the on-disk version past what this build reads.
    {
        std::ifstream in(doc);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        nlohmann::json parsed = nlohmann::json::parse(buffer.str());
        parsed["version"] = DOCUMENT_VERSION + 1;
        std::ofstream out(doc);
        out << parsed.dump();
    }
    std::ostringstream out;
    check(cmd_validate(out, doc) != EXIT_OK, "a newer document is refused");
    check(contains(out.str(), "newer build"), "the reason names the version gap");
}

void test_flatten()
{
    const std::string doc = make_document();
    std::ostringstream out;
    check(cmd_flatten(out, doc, std::nullopt) == EXIT_OK, "flatten exits 0");
    const std::string text = out.str();
    check(contains(text, "clips: 2"), "flatten counts the clips");
    check(contains(text, "1 video start=0 duration=10 source=/footage/a.mp4"),
          "flatten lists the first clip");
    check(contains(text, "2 video start=10 duration=1/3 source=/footage/b.mp4"),
          "flatten lists the second clip, exact fraction intact");
}

void test_render_refuses_an_unreadable_project()
{
    const std::string garbage = write_temp("render-garbage.json", "not json");
    std::ostringstream out;
    check(cmd_render(out, garbage, "/tmp/genesis-render.mp4", std::nullopt) != EXIT_OK,
          "render refuses an unreadable project");
    check(contains(out.str(), "error: "), "the refusal is an error");
}

void test_render_refuses_an_unknown_preset()
{
    const std::string doc = make_document();
    std::ostringstream out;
    check(cmd_render(out, doc, "/tmp/genesis-render.mp4", std::string("nope")) != EXIT_OK,
          "render refuses an unknown preset");
    check(contains(out.str(), "unknown preset 'nope'"), "the refusal names the unknown preset");
}

void test_render_refuses_an_empty_timeline()
{
    const std::string doc = make_empty_document();
    std::ostringstream out;
    check(cmd_render(out, doc, "/tmp/genesis-render.mp4", std::nullopt) != EXIT_OK,
          "render refuses a project with no clips");
    check(contains(out.str(), "timeline has no clips"), "the refusal names the empty timeline");
}

void test_packs()
{
    const std::string manifest = write_temp("effect.toml", R"(
        [effect]
        id = "example.demo"
        name = "Demo"
        kind = "effect"
        version = 3

        [[param]]
        key = "amount"
        label = "Amount"
        type = "float"
        min = 0
        max = 10
        default = 2.5

        [[param]]
        key = "count"
        label = "Count"
        type = "int"
        min = 1
        max = 9
        default = 3

        [wgsl]
        entry = "effect.wgsl"

        [[wgsl.pass]]
        target = "across"
    )");
    std::ostringstream out;
    check(cmd_packs(out, manifest) == EXIT_OK, "packs exits 0");
    const std::string text = out.str();
    check(contains(text, "id: example.demo"), "packs names the pack");
    check(contains(text, "kind: effect"), "packs reports the kind");
    check(contains(text, "version: 3"), "packs reports the version");
    check(contains(text, "parameters: 2"), "packs counts the parameters");
    check(contains(text, "amount float [0, 10] default=2.5"), "packs lists a float parameter");
    check(contains(text, "passes: 1"), "packs counts the passes");
    check(contains(text, "across"), "packs lists the pass target");
}

void test_packs_refuses_a_bad_manifest()
{
    const std::string manifest = write_temp("bad-effect.toml", "[effect]\n");
    std::ostringstream out;
    check(cmd_packs(out, manifest) != EXIT_OK, "a bad manifest is refused");
    check(contains(out.str(), "error: "), "the refusal is an error");
}

void test_packs_trial_valid()
{
    const std::string dir =
            write_pack_dir_with_body("trial-good", pack_manifest("cli.trial", 1), valid_body);
    std::ostringstream out;
    check(cmd_packs_trial(out, dir, false) == EXIT_OK, "trial exits 0 for a valid Pack");
    const std::string text = out.str();
    check(contains(text, "manifest: ok"), "trial validates the manifest");
    check(contains(text, "validated, not trusted"), "trial reports validated, not trusted");
    // The shader phase either compiles or is skipped on a host with no
    // headless GL context; both keep a valid manifest passing.
    check(contains(text, "shader: compiled") || contains(text, "shader: skipped"),
          "trial reports the shader verdict");
    check(contains(text, "extensions install"), "trial spells out the install path");
}

void test_packs_trial_validate_only()
{
    // No body is written: --validate-only never reads the shader.
    const std::string dir = write_pack_dir("trial-manifest-only", pack_manifest("cli.trial", 1));
    std::ostringstream out;
    check(cmd_packs_trial(out, dir, true) == EXIT_OK, "validate-only exits 0");
    const std::string text = out.str();
    check(contains(text, "shader: skipped (--validate-only)"), "validate-only skips the shader");
    check(contains(text, "validated, not trusted"), "validate-only still reports not trusted");
}

void test_packs_trial_refuses_a_bad_manifest()
{
    const std::string dir = write_pack_dir("trial-bad-manifest", "[effect]\n");
    std::ostringstream out;
    check(cmd_packs_trial(out, dir, false) != EXIT_OK, "a bad manifest is refused by trial");
    check(contains(out.str(), "error: "), "the refusal is an error");
}

void test_packs_trial_catches_a_bad_shader()
{
    const std::string bad_body = "void main () { gl_FragColor = texture2D "
                                 "(tex, v_texcoord) + broken; }\n";
    const std::string dir =
            write_pack_dir_with_body("trial-bad-shader", pack_manifest("cli.trial", 1), bad_body);
    std::ostringstream out;
    const int code = cmd_packs_trial(out, dir, false);
    const std::string text = out.str();
    if (contains(text, "failed to compile")) {
        // A headless context exists and the broken body was refused.
        check(code != EXIT_OK, "a broken shader fails the trial");
    } else if (contains(text, "skipped")) {
        // No headless context: the shader phase was skipped, so the manifest
        // alone passes.
        check(code == EXIT_OK, "a skipped shader phase still passes");
    } else {
        check(false, "the trial reports a compile failure or a skip");
    }
}

void test_commands()
{
    std::ostringstream out;
    check(cmd_commands(out) == EXIT_OK, "commands exits 0");
    const std::string text = out.str();
    std::size_t lines = 0;
    for (char c : text) {
        if (c == '\n') {
            ++lines;
        }
    }
    check(lines == 45, "commands lists all forty-five verbs");
    check(contains(text, "AddClip  "), "commands includes AddClip");
    check(contains(text, "SplitClips  "), "commands includes SplitClips");
    check(contains(text, "UpdateMediaPath  "), "commands includes UpdateMediaPath");
    // Sorted by name: AddClip precedes AddClipAtFirstFree.
    check(text.find("AddClip  ") < text.find("AddClipAtFirstFree  "), "commands is sorted by name");
}

void test_clear_cache()
{
    const std::filesystem::path base =
            std::filesystem::temp_directory_path() / "genesis-cli-tests" / "clear-cache";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base / "masks");

    const auto write = [](const std::filesystem::path &path, std::uintmax_t size, int age) {
        std::ofstream out(path, std::ios::binary);
        for (std::uintmax_t i = 0; i < size; ++i) {
            out.put('x');
        }
        out.close();
        const auto now = std::filesystem::file_time_type::clock::now();
        std::filesystem::last_write_time(path, now - std::chrono::seconds(age));
    };
    write(base / "oldest.peaks", 40, 30);
    write(base / "oldest.peaks.key", 4, 30);
    write(base / "masks" / "newest.mask", 20, 10);

    // Without --cap: every entry (and its sidecar) is removed.
    std::ostringstream cleared;
    check(cmd_clear_cache(cleared, base.string(), std::nullopt) == EXIT_OK,
          "clear-cache exits 0 without a cap");
    const std::string clear_text = cleared.str();
    check(contains(clear_text, "removed: 2 files"), "clear removes both data files");
    check(contains(clear_text, "after: 0 bytes"), "clear reports zero bytes remaining");
    check(!std::filesystem::exists(base / "oldest.peaks"), "the clear removed the data file");
    check(!std::filesystem::exists(base / "oldest.peaks.key"), "the clear removed the sidecar");

    // With --cap: only the oldest entries are evicted to fit the cap.
    write(base / "oldest.peaks", 40, 30);
    write(base / "masks" / "newest.mask", 20, 10);
    std::ostringstream capped;
    check(cmd_clear_cache(capped, base.string(), std::uint64_t{ 25 }) == EXIT_OK,
          "clear-cache exits 0 with a cap");
    const std::string cap_text = capped.str();
    check(contains(cap_text, "evicted: 1 files"), "the sweep evicts the one oldest file");
    check(contains(cap_text, "after: 20 bytes"), "the sweep reports the remaining bytes");
    check(!std::filesystem::exists(base / "oldest.peaks"), "the sweep evicted the oldest file");
    check(std::filesystem::exists(base / "masks" / "newest.mask"),
          "the sweep kept the newest file");

    std::filesystem::remove_all(base);
}

void test_extensions_install_and_list()
{
    const std::string pack = write_extension_dir("blur-v1", native_manifest("cli.blur", 1));
    const std::filesystem::path base =
            std::filesystem::temp_directory_path() / "genesis-cli-tests" / "store";
    std::filesystem::remove_all(base);
    const std::string store = base.string();

    std::ostringstream installed;
    check(cmd_extensions_install(installed, pack, store, "builtin") == EXIT_OK,
          "install exits 0 for a trusted origin");
    check(contains(installed.str(), "installed: "), "install reports the installed root");

    std::ostringstream listed;
    check(cmd_extensions_list(listed, store) == EXIT_OK, "list exits 0");
    const std::string text = listed.str();
    check(contains(text, "packs: 1"), "list counts the installed Pack");
    check(contains(text, "cli.blur"), "list names the Pack");
    check(contains(text, "1 "), "list reports the version");

    std::ostringstream refused;
    check(cmd_extensions_install(refused, pack, store, "user") != EXIT_OK,
          "an untrusted origin is refused");
    check(contains(refused.str(), "not trusted"), "the refusal names the trust failure");

    std::ostringstream unknown;
    check(cmd_extensions_install(unknown, pack, store, "nope") != EXIT_OK,
          "an unknown origin is refused");
    check(contains(unknown.str(), "unknown origin"), "the refusal names the unknown origin");

    std::filesystem::remove_all(base);
}

void test_extensions_catalog_persists_revocations()
{
    const std::filesystem::path base =
            std::filesystem::temp_directory_path() / "genesis-cli-tests" / "catalog-store";
    std::filesystem::remove_all(base);
    const std::string store = base.string();

    // A one-entry catalog whose `revoked` list names an id: listing it must
    // persist the revocation into the store, not just report it.
    const nlohmann::json catalog = {
        { "entries",
          nlohmann::json::array({ nlohmann::json{
                  { "id", "cli.revoked" },
                  { "name", "Revoked" },
                  { "version", "1" },
                  { "source",
                    nlohmann::json{ { "type", "dir" }, { "url", "/tmp/does-not-exist" } } } } }) },
        { "revoked",
          nlohmann::json::array(
                  { nlohmann::json{ { "id", "cli.revoked" }, { "reason", "malware" } } }) }
    };
    const std::function<std::optional<std::string>(const std::string &)> fetcher =
            [&](const std::string &) -> std::optional<std::string> { return catalog.dump(); };

    std::ostringstream out;
    check(cmd_extensions_catalog(out, "https://example.com/catalog.json", store, fetcher)
                  == EXIT_OK,
          "catalog lists exit 0");
    check(contains(out.str(), "cli.revoked"), "the entry is listed");

    const std::optional<genesis::extensions::Revocation> revocation =
            genesis::extensions::revoked_at(store, "cli.revoked", "1");
    check(revocation.has_value(), "the catalog list verb persists the revocation");
    if (revocation) {
        check(revocation->reason.has_value() && *revocation->reason == "malware",
              "with the publisher's reason");
    }

    std::filesystem::remove_all(base);
}

void test_extensions_trust()
{
    std::ostringstream all;
    check(cmd_extensions_trust(all, std::nullopt) == EXIT_OK, "trust exits 0 with no origin");
    check(contains(all.str(), "builtin: trusted"), "builtin is trusted");
    check(contains(all.str(), "curated: trusted"), "curated is trusted");
    check(contains(all.str(), "user: not trusted"), "user is not trusted");
    check(contains(all.str(), "execute: no"), "user may not execute a shader");

    std::ostringstream one;
    check(cmd_extensions_trust(one, std::string("user")) == EXIT_OK,
          "trust exits 0 for one origin");
    check(contains(one.str(), "user: not trusted"), "the named origin is reported");

    std::ostringstream bad;
    check(cmd_extensions_trust(bad, std::string("nope")) != EXIT_OK,
          "an unknown origin is refused");
}

void test_extensions_remove_and_restore()
{
    const std::string pack = write_extension_dir("rm-v1", native_manifest("cli.remove", 1));
    const std::filesystem::path base =
            std::filesystem::temp_directory_path() / "genesis-cli-tests" / "rm-store";
    std::filesystem::remove_all(base);
    const std::string store = base.string();

    std::ostringstream installed;
    check(cmd_extensions_install(installed, pack, store, "builtin") == EXIT_OK,
          "install exits 0 before remove");

    std::ostringstream removed;
    check(cmd_extensions_remove(removed, store, "cli.remove") == EXIT_OK, "remove exits 0");
    check(contains(removed.str(), "removed: cli.remove"), "remove reports the id");
    check(!std::filesystem::exists(base / "cli.remove"), "remove deletes the installed copy");
    check(genesis::extensions::removed(store, "cli.remove"), "remove records the tombstone");

    // Removing an id with no installed copy still tombstones and succeeds.
    std::ostringstream absent;
    check(cmd_extensions_remove(absent, store, "cli.absent") == EXIT_OK,
          "removing an absent id still succeeds");
    check(genesis::extensions::removed(store, "cli.absent"), "and records its tombstone");

    std::ostringstream restored;
    check(cmd_extensions_restore(restored, store, "cli.remove") == EXIT_OK, "restore exits 0");
    check(contains(restored.str(), "restored: cli.remove"), "restore reports the id");
    check(!genesis::extensions::removed(store, "cli.remove"), "restore clears the tombstone");

    std::filesystem::remove_all(base);
}

void test_extensions_enable_and_disable()
{
    const std::filesystem::path base =
            std::filesystem::temp_directory_path() / "genesis-cli-tests" / "toggle-store";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    const std::string store = base.string();
    write_native_extension(store, "dep.base", 1);
    {
        std::ofstream out(base / "origins.json");
        out << "{\"dep.base\": \"builtin\"}\n";
    }

    std::ostringstream disabled;
    check(cmd_extensions_disable(disabled, store, "dep.base") == EXIT_OK, "disable exits 0");
    check(contains(disabled.str(), "disabled: dep.base"), "disable reports the id");
    check(genesis::extensions::DisabledList(base).disabled("dep.base"),
          "the id lands on the disabled list");

    std::ostringstream enabled;
    check(cmd_extensions_enable(enabled, store, "dep.base") == EXIT_OK, "enable exits 0");
    check(contains(enabled.str(), "enabled: dep.base"), "enable reports the id");
    check(!genesis::extensions::DisabledList(base).disabled("dep.base"),
          "the id leaves the disabled list");

    std::filesystem::remove_all(base);
}

void test_extensions_list_requires_dependents()
{
    const std::filesystem::path base =
            std::filesystem::temp_directory_path() / "genesis-cli-tests" / "deps-store";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    const std::string store = base.string();
    write_native_extension(store, "dep.child", 1, { "dep.base" });
    write_native_extension(store, "dep.base", 1);
    {
        std::ofstream out(base / "origins.json");
        out << "{\"dep.child\": \"builtin\", \"dep.base\": \"builtin\"}\n";
    }

    std::ostringstream listed;
    check(cmd_extensions_list(listed, store) == EXIT_OK, "list exits 0");
    const std::string text = listed.str();
    check(contains(text, "requires: dep.base"), "list reports the dependent's requires");
    check(contains(text, "dependents: dep.child"), "list reports the dependency's dependents");

    std::filesystem::remove_all(base);
}

void test_api_version_round_trip()
{
    std::istringstream in("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"version\"}\n");
    std::ostringstream out;
    check(cmd_api(in, out) == EXIT_OK, "api exits 0 at end of input");
    const nlohmann::json reply = nlohmann::json::parse(out.str());
    check(reply["jsonrpc"] == "2.0", "api replies JSON-RPC 2.0");
    check(reply["id"] == 1, "api echoes the id");
    check(reply.contains("result"), "api returns a result");
    check(reply["result"]["apiVersion"] == "0.2", "api result carries the contract version");

    // A malformed line is a parse error, not a dropped request.
    std::istringstream bad("not json\n");
    std::ostringstream errored;
    check(cmd_api(bad, errored) == EXIT_OK, "api survives a bad line");
    const nlohmann::json failure = nlohmann::json::parse(errored.str());
    check(failure["error"]["code"] == -32700, "a bad line is a parse error");

    // Unknown methods are refused by name.
    std::istringstream unknown("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"nope\"}\n");
    std::ostringstream refused;
    check(cmd_api(unknown, refused) == EXIT_OK, "api survives an unknown method");
    const nlohmann::json missing = nlohmann::json::parse(refused.str());
    check(missing["error"]["code"] == -32601, "an unknown method is method-not-found");
}

void test_api_probe_initializes_gstreamer()
{
    // The api verb's probe seam reaches real GStreamer, so cmd_api must bring
    // the library up itself. This runs before any other test calls gst_init,
    // so a missing gst_init in cmd_api leaves the library uninitialized here.
    check(!gst_is_initialized(), "GStreamer starts uninitialized in this process");
    const std::string fixture = write_temp("probe-fixture.webm", "");
    std::istringstream in("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"media.probe\","
                          "\"params\":{\"path\":\""
                          + fixture + "\"}}\n");
    std::ostringstream out;
    check(cmd_api(in, out) == EXIT_OK, "media.probe over stdio exits 0");
    check(gst_is_initialized(), "cmd_api initializes GStreamer for the probe seam");
    std::filesystem::remove(fixture);
}

void test_api_edit_apply_round_trip()
{
    // A command sent over stdio is decoded and applied, and the reply is the
    // editor view. addTrack is the simplest observable mutation.
    std::istringstream in("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"edit.apply\","
                          "\"params\":{\"path\":\"\",\"command\":{\"op\":\"addTrack\"}}}\n");
    std::ostringstream out;
    check(cmd_api(in, out) == EXIT_OK, "edit.apply over stdio exits 0");
    const nlohmann::json reply = nlohmann::json::parse(out.str());
    check(reply["id"] == 3, "edit.apply echoes the id");
    check(reply.contains("result"), "edit.apply returns a result");
    check(reply["result"].contains("project"), "edit.apply returns the editor view");

    // An undecodable command is an Invalid request, not a crash.
    std::istringstream bad_in("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"edit.apply\","
                              "\"params\":{\"path\":\"\",\"command\":{\"op\":\"noSuchOp\"}}}\n");
    std::ostringstream bad_out;
    check(cmd_api(bad_in, bad_out) == EXIT_OK, "a bad command is survived");
    const nlohmann::json bad = nlohmann::json::parse(bad_out.str());
    check(bad["error"]["code"] == -32600, "an undecodable command is invalid-request");

    // A missing command field is invalid too.
    std::istringstream missing_in("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"edit.apply\","
                                  "\"params\":{\"path\":\"\"}}\n");
    std::ostringstream missing_out;
    check(cmd_api(missing_in, missing_out) == EXIT_OK, "a missing command is survived");
    const nlohmann::json missing = nlohmann::json::parse(missing_out.str());
    check(missing["error"]["code"] == -32600, "a missing command is invalid-request");
}

void test_api_oversize_line_refused()
{
    // A request line longer than the 1 MiB cap is answered with the parse
    // error reply and discarded through its newline, without the parser ever
    // seeing the (truncated) bytes.
    std::string big = "{\"m\":\"";
    big.append(kMaxJsonRequestBytes + 1, 'a');
    big += "\"}\n";
    // A well-formed request after it still gets a real reply, proving the
    // reader recovered onto the next line.
    big += "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"version\"}\n";

    std::istringstream in(big);
    std::ostringstream out;
    check(cmd_api(in, out) == EXIT_OK, "api exits 0 after an oversized line");

    std::istringstream replies(out.str());
    std::string first;
    std::getline(replies, first);
    const nlohmann::json oversize = nlohmann::json::parse(first);
    check(oversize["error"]["code"] == -32700,
          "an oversized line maps to the JSON-RPC parse error");

    std::string second;
    std::getline(replies, second);
    const nlohmann::json version = nlohmann::json::parse(second);
    check(version["id"] == 1, "the next line is still answered normally");
}

void test_config_export_and_import()
{
    const std::string pack = write_extension_dir("cfg-v1", native_manifest("cli.cfg", 1));
    const std::filesystem::path base =
            std::filesystem::temp_directory_path() / "genesis-cli-tests" / "cfg-store";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    const std::string store = base.string();

    // A builtin-origin Pack install records a Dir source (its absolute path),
    // so the exported profile carries a source record.
    std::ostringstream installed;
    check(cmd_extensions_install(installed, pack, store, "builtin") == EXIT_OK,
          "install exits 0 before export");

    const std::string file = (base / "profile.json").string();
    std::ostringstream exported;
    check(cmd_config_export(exported, file, store) == EXIT_OK, "config export exits 0");
    check(contains(exported.str(), "exported: " + file), "export reports the file");

    std::ifstream in(file);
    const nlohmann::json profile = nlohmann::json::parse(in);
    check(!profile.contains("format"), "the exported profile carries no format key");
    check(profile["extensions"].size() == 1, "the profile lists the one extension");
    check(profile["extensions"][0]["id"] == "cli.cfg", "the extension id round-trips");
    check(profile["extensions"][0]["source"].is_object(), "the source record is present");

    // Importing with --no-install applies settings (none here) and skips every
    // extension, reporting the skip.
    std::ostringstream imported;
    check(cmd_config_import(imported, file, store, true) == EXIT_OK, "config import exits 0");
    check(contains(imported.str(), "extensions skipped (--no-install)"),
          "--no-install reports the skip");

    // A corrupt profile is refused, not half-applied.
    const std::string bad = write_temp("bad-profile.json", "not json");
    std::ostringstream refused;
    check(cmd_config_import(refused, bad, store, false) != EXIT_OK, "a corrupt profile is refused");
    check(contains(refused.str(), "not valid JSON"), "the refusal names the bad JSON");

    std::filesystem::remove_all(base);
}

void test_cli_services_seam()
{
    const genesis::api::Services services = cli_services();

    // Both engine-bound seams are wired: the CLI links the adapter for
    // `render`, so a media probe and the effect catalogue answer for real
    // rather than a typed refusal.
    check(static_cast<bool>(services.probe), "the probe seam is wired");
    check(static_cast<bool>(services.catalogue), "the catalogue seam is wired");

    // The catalogue seam is host-only (no GStreamer): it reads the builtin
    // catalogue and the kind filter narrows it.
    const auto all = services.catalogue(std::nullopt);
    check(!all.empty(), "the catalogue seam lists the builtin packs");
    const auto filters = services.catalogue(std::string("filter"));
    check(!filters.empty(), "the catalogue seam returns filters");
    for (const auto &pack : filters) {
        check(pack.kind == genesis::effects::Kind::Filter, "the filter narrows by kind");
    }
    check(filters.size() < all.size(), "the filter is a proper subset");

    // The probe seam reaches the real engine probe: a missing file is a clean
    // nullopt, not a crash. No media fixture is needed for the refusal path.
    const auto probed = services.probe("/definitely/not/a/media/file.webm");
    check(!probed.has_value(), "the probe seam refuses a missing file");
}

// Exercises every one of the 45 command verbs through the edit.apply JSON-RPC
// path in a single cmd_api session, asserting each returns a result.
void test_api_exercises_every_command_verb()
{
    using nlohmann::json;

    auto media_item = [](const std::string &path, const std::string &duration,
                         bool has_audio = true) -> json {
        return json{ { "path", path },         { "name", path.substr(path.find_last_of('/') + 1) },
                     { "duration", duration }, { "kind", "video" },
                     { "width", 1920 },        { "height", 1080 },
                     { "frameRate", 30.0 },    { "videoCodec", "h264" },
                     { "hasAudio", has_audio } };
    };

    auto make_rpc = [](int id, const std::string &method, const json &params) -> std::string {
        return json{
            { "jsonrpc", "2.0" }, { "id", id }, { "method", method }, { "params", params }
        }.dump() + "\n";
    };

    auto make_edit = [&](int id, const json &command) -> std::string {
        return make_rpc(id, "edit.apply", json{ { "path", "" }, { "command", command } });
    };

    // Ordered per the verb list, satisfying every precondition in sequence.
    // Known ids: m1 (media), c1-c4 (clips), T5 (track), TL2 (timeline), f1 (font).
    std::string body;
    const json still = json{ { "path", "/tmp/ges-spike/freeze.png" },
                             { "name", "freeze.png" },
                             { "kind", "image" },
                             { "width", 1920 },
                             { "height", 1080 } };
    const json video_settings =
            json{ { "width", 1920 }, { "height", 1080 }, { "rateNum", 30 }, { "rateDen", 1 } };

    // 1. AddMedia
    body += make_edit(
            1,
            json{ { "op", "addMedia" }, { "item", media_item("/tmp/ges-spike/a.webm", "2/1") } });
    body += make_edit(2,
                      json{ { "op", "addClip" },
                            { "mediaId", "m1" },
                            { "trackId", "T2" },
                            { "start", "0/1" },
                            { "ripple", false } });
    body += make_edit(
            3, json{ { "op", "addClipAtFirstFree" }, { "mediaId", "m1" }, { "start", "0/1" } });
    body += make_edit(4,
                      json{ { "op", "addTextClip" },
                            { "trackId", nullptr },
                            { "above", false },
                            { "start", "0/1" } });
    body += make_edit(5,
                      json{ { "op", "addLayerClip" },
                            { "trackId", nullptr },
                            { "start", "0/1" },
                            { "effectId", "genesis.blur" },
                            { "name", "Layer" } });
    body += make_edit(6, json{ { "op", "addTrack" } });
    body += make_edit(7,
                      json{ { "op", "addFont" },
                            { "family", "TestFont" },
                            { "path", "/tmp/ges-spike/test.ttf" } });
    body += make_edit(8,
                      json{ { "op", "setClipCutout" }, { "clipId", "c2" }, { "cutout", nullptr } });
    body += make_edit(9,
                      json{ { "op", "addCutoutStroke" },
                            { "clipId", "c2" },
                            { "stroke",
                              json{ { "tool", "brush" },
                                    { "size", 10.0 },
                                    { "points",
                                      json::array({ json::array({ 0.0, 0.0 }),
                                                    json::array({ 0.5, 0.5 }) }) } } } });
    body += make_edit(10,
                      json{ { "op", "setClipKey" },
                            { "clipId", "c2" },
                            { "property", "scale" },
                            { "at", 0.0 },
                            { "value", 1.5 },
                            { "ease", "inOut" } });
    body += make_edit(11,
                      json{ { "op", "clearClipKey" },
                            { "clipId", "c2" },
                            { "property", "scale" },
                            { "at", 0.0 } });
    body += make_edit(
            12, json{ { "op", "clearClipKeys" }, { "clipId", "c2" }, { "property", "scale" } });
    body += make_edit(13, json{ { "op", "setClipSpeed" }, { "clipId", "c2" }, { "speed", 2.0 } });
    body += make_edit(
            14, json{ { "op", "setClipSpeedCurve" }, { "clipId", "c2" }, { "curve", nullptr } });
    body += make_edit(15,
                      json{ { "op", "setClipTransform" },
                            { "clipId", "c2" },
                            { "scale", 1.5 },
                            { "offsetX", 0.1 } });
    body += make_edit(16,
                      json{ { "op", "updateClip" },
                            { "clipId", "c2" },
                            { "patch", json{ { "name", "Renamed" } } } });
    body += make_edit(17,
                      json{ { "op", "trimClip" },
                            { "clipId", "c2" },
                            { "edge", "end" },
                            { "delta", "-1/1" },
                            { "ripple", false } });
    body += make_edit(
            18,
            json{ { "op", "moveClips" },
                  { "moves",
                    json::array({ json{
                            { "clipId", "c2" }, { "start", "1/1" }, { "trackId", "T2" } } }) } });
    body += make_edit(19,
                      json{ { "op", "setEffectKey" },
                            { "clipId", "c5" },
                            { "entry", 0 },
                            { "key", "amount" },
                            { "at", 0.0 },
                            { "value", 0.5 },
                            { "ease", "inOut" } });
    body += make_edit(20,
                      json{ { "op", "clearEffectKey" },
                            { "clipId", "c5" },
                            { "entry", 0 },
                            { "key", "amount" },
                            { "at", 0.0 } });
    body += make_edit(21,
                      json{ { "op", "clearEffectKeys" },
                            { "clipId", "c5" },
                            { "entry", 0 },
                            { "key", "amount" } });
    body += make_edit(22, json{ { "op", "detachAudio" }, { "clipId", "c2" } });
    body += make_edit(23, json{ { "op", "reattachAudio" }, { "clipId", "c2" } });
    body += make_edit(24,
                      json{ { "op", "splitClips" },
                            { "clipIds", json::array({ "c3" }) },
                            { "time", "1/1" } });
    body += make_edit(25,
                      json{ { "op", "mergeClips" }, { "clipIds", json::array({ "c3", "c8" }) } });
    body += make_edit(26,
                      json{ { "op", "freezeFrame" },
                            { "clipId", "c3" },
                            { "time", "1/1" },
                            { "still", still } });
    body += make_edit(
            27,
            json{ { "op", "setMediaPlaceholder" }, { "mediaId", "m1" }, { "placeholder", true } });
    body += make_edit(28,
                      json{ { "op", "fillSlot" },
                            { "mediaId", "m1" },
                            { "item", media_item("/tmp/ges-spike/filled.webm", "3/1") } });
    body += make_edit(29,
                      json{ { "op", "updateMediaPath" },
                            { "mediaId", "m1" },
                            { "newPath", "/tmp/ges-spike/relinked.webm" } });
    body += make_edit(
            30, json{ { "op", "setMediaColorRange" }, { "mediaId", "m1" }, { "range", nullptr } });
    body += make_edit(31,
                      json{ { "op", "replaceClipMedia" },
                            { "clipId", "c2" },
                            { "item", media_item("/tmp/ges-spike/replaced.webm", "4/1") },
                            { "sourceStart", nullptr } });
    body += make_edit(32,
                      json{ { "op", "removeClips" },
                            { "clipIds", json::array({ "c2", "c3", "c4" }) },
                            { "ripple", false } });
    body += make_edit(33, json{ { "op", "removeMedia" }, { "mediaId", "m1" } });
    body += make_edit(34, json{ { "op", "addTimeline" } });
    body += make_edit(35, json{ { "op", "selectTimeline" }, { "timelineId", "TL1" } });
    body += make_edit(36, json{ { "op", "removeFont" }, { "family", "TestFont" } });
    body += make_edit(37,
                      json{ { "op", "setTrackFlag" },
                            { "trackId", "T4" },
                            { "flag", "muted" },
                            { "value", true } });
    body += make_edit(38,
                      json{ { "op", "setTimelineVideo" },
                            { "timelineId", "TL1" },
                            { "video", video_settings } });
    body += make_edit(
            39, json{ { "op", "renameTimeline" }, { "timelineId", "TL1" }, { "name", "Renamed" } });
    body += make_edit(40,
                      json{ { "op", "moveTimeline" }, { "timelineId", "TL2" }, { "index", 0 } });
    body += make_edit(41, json{ { "op", "removeTimeline" }, { "timelineId", "TL2" } });
    body += make_edit(42, json{ { "op", "removeTrack" }, { "trackId", "T5" } });
    body += make_edit(43,
                      json{ { "op", "batch" },
                            { "commands",
                              json::array({ json{ { "op", "addTrack" } },
                                            json{ { "op", "setTrackFlag" },
                                                  { "trackId", "T4" },
                                                  { "flag", "visible" },
                                                  { "value", false } } }) } });
    body += make_edit(44,
                      json{ { "op", "setClipReverse" }, { "clipId", "c5" }, { "reverse", true } });
    body += make_edit(45, json{ { "op", "setClipPan" }, { "clipId", "c5" }, { "pan", -0.5 } });

    std::istringstream in(body);
    std::ostringstream out;
    check(cmd_api(in, out) == EXIT_OK, "cmd_api exits 0 for the full verb suite");

    // Parse each reply and verify it carries a result, not an error.
    std::string line;
    int verb_count = 0;
    std::istringstream replies(out.str());
    while (std::getline(replies, line)) {
        if (line.empty()) {
            continue;
        }
        const json reply = json::parse(line);
        const int id = reply["id"].get<int>();
        check(reply.contains("result"), "verb " + std::to_string(id) + " returns a result");
        check(!reply.contains("error"), "verb " + std::to_string(id) + " does not error");
        ++verb_count;
    }
    check(verb_count == 45, "all 45 verbs returned a result");
}

} // namespace

int main()
{
    // No gst_init here: cmd_api must initialize GStreamer itself, and the
    // probe test below runs first to pin that. Every other entry point
    // (cmd_render, cmd_packs_trial) also calls gst_init on its own.
    test_api_probe_initializes_gstreamer();

    test_inspect_human();
    test_inspect_json();
    test_validate_ok();
    test_validate_refuses_garbage();
    test_validate_refuses_a_newer_build();
    test_flatten();
    test_render_refuses_an_unreadable_project();
    test_render_refuses_an_unknown_preset();
    test_render_refuses_an_empty_timeline();
    test_packs();
    test_packs_refuses_a_bad_manifest();
    test_packs_trial_valid();
    test_packs_trial_validate_only();
    test_packs_trial_refuses_a_bad_manifest();
    test_packs_trial_catches_a_bad_shader();
    test_commands();
    test_clear_cache();
    test_extensions_install_and_list();
    test_extensions_catalog_persists_revocations();
    test_extensions_trust();
    test_extensions_remove_and_restore();
    test_extensions_enable_and_disable();
    test_extensions_list_requires_dependents();
    test_api_version_round_trip();
    test_api_edit_apply_round_trip();
    test_api_oversize_line_refused();
    test_config_export_and_import();
    test_cli_services_seam();
    test_api_exercises_every_command_verb();

    return genesis::test::summary();
}
