// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "cli/Commands.h"
#include "cli/JsonRpc.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <istream>
#include <memory>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <span>

#include <gst/gst.h>
#include <gst/gl/egl/gstgldisplay_egl.h>
#include <gst/gl/gl.h>
#include <gst/gl/gstglslstage.h>
#include <ges/ges.h>

#include "adapters/engine/Export.h"
#include "adapters/engine/ges/effects/ShaderChain.h"
#include "api/Api.h"
#include "core/ClipTimeline.h"
#include "core/JsonGuard.h"
#include "effects/Pack.h"
#include "effects/PackLoader.h"
#include "effects/PackSource.h"
#include "effects/Resolve.h"
#include "extensions/Catalog.h"
#include "extensions/Config.h"
#include "extensions/Install.h"
#include "extensions/Manager.h"
#include "extensions/RemovedList.h"
#include "extensions/Seed.h"
#include "extensions/SourceInstall.h"
#include "extensions/Sources.h"
#include "extensions/Subprocess.h"
#include "extensions/Trust.h"
#include "project/model/Effects.h"
#include "project/Document.h"
#include "project/Editor.h"
#include "project/Project.h"
#include "project/command/AudioCommands.h"
#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/command/PropertyCommands.h"
#include "project/command/TimelineCommands.h"
#include "project/command/TrackCommands.h"
#include "project/model/Clip.h"
#include "project/model/Speed.h"
#include "project/model/Timeline.h"
#include "render/ExportClip.h"
#include "render/ExportPresets.h"
#include "render/ExportSpec.h"
#include "render/Flatten.h"
#include "render/Resolve.h"

namespace genesis::cli {

namespace {

using nlohmann::json;

namespace project = genesis::project;
namespace render = genesis::render;
namespace effects = genesis::effects;
namespace extensions = genesis::extensions;
namespace api = genesis::api;
namespace engine = genesis::adapters::engine;
namespace ges = genesis::adapters::engine::ges;
namespace core = genesis::core;

// The CLI's own version, matching the `--version` flag in main.cpp. It is
// recorded in an exported profile's `appVersion` so an import can surface a
// version mismatch; the headless CLI has no app controller to ask.
inline constexpr const char *kCliVersion = "0.1.0";

// --- `packs trial` -------------------------------------------------------
//
// A trial vets a candidate Pack directory without executing anything: it
// validates the manifest and compiles the GLSL body headlessly. The compile is
// the whole of the GL work - one fragment stage, compiled and never drawn - so
// the phase is bounded by the driver's compiler rather than by any render loop.

// What the shader phase decided. `Compiled` and `Failed` apply to a picture
// Pack whose body the host fragment path can express; `Skipped` is a Pack with
// no body to compile on that path (audio, transition, named intermediates) or
// a host with no headless GL context. A skipped phase never fails a valid
// manifest.
enum class ShaderVerdict { Compiled, Failed, Skipped };

struct ShaderReport
{
    ShaderVerdict verdict = ShaderVerdict::Skipped;
    std::string detail; // the compile log on failure, the skip reason otherwise
};

// The outcome of one headless fragment compile. `available` is false when no
// offscreen GL context could be created at all (no GPU, no llvmpipe), so the
// caller degrades to manifest-only rather than failing a valid Pack on a
// machine that cannot compile shaders.
struct CompileResult
{
    bool available = false;
    bool compiled = false;
    std::string log;
};

// Compiles `fragment` on a fresh offscreen EGL context and releases it. The
// stage is compiled, never linked into a program or drawn, so nothing beyond
// the compiler runs. `gst_init` is idempotent and GL needs GStreamer up first.
CompileResult compile_fragment(const std::string &fragment)
{
    CompileResult result;
    gst_init(nullptr, nullptr);

    GstGLDisplay *display = reinterpret_cast<GstGLDisplay *>(gst_gl_display_egl_new());
    if (display == nullptr) {
        return result;
    }
    GstGLContext *context = gst_gl_context_new(display);
    GError *error = nullptr;
    if (context == nullptr || !gst_gl_context_create(context, nullptr, &error)) {
        result.log = error != nullptr ? error->message : "no headless GL context";
        g_clear_error(&error);
        gst_clear_object(&context);
        gst_object_unref(display);
        return result;
    }
    result.available = true;

    GstGLSLStage *stage = gst_glsl_stage_new_default_fragment(context);
    if (stage == nullptr) {
        result.log = "cannot create the fragment stage";
    } else {
        const char *source[1] = { fragment.c_str() };
        (void)gst_glsl_stage_set_strings(stage, GST_GLSL_VERSION_NONE, GST_GLSL_PROFILE_NONE, 1,
                                         source);
        GError *compile_error = nullptr;
        result.compiled = gst_glsl_stage_compile(stage, &compile_error) != FALSE;
        if (!result.compiled) {
            result.log = compile_error != nullptr ? compile_error->message
                                                  : "the fragment shader failed to compile";
            g_clear_error(&compile_error);
        }
        gst_object_unref(stage);
    }
    gst_object_unref(context);
    gst_object_unref(display);
    return result;
}

// Loads the pack's body, composes the fragment the engine would run, and
// compiles it headlessly. A picture Pack that resolves gets a verdict; the
// three kinds the fragment path cannot express are reported as skips with the
// reason, so a valid manifest still passes.
ShaderReport trial_shader(const effects::Pack &pack, const std::filesystem::path &dir)
{
    if (pack.kind == effects::Kind::Audio) {
        return { ShaderVerdict::Skipped, "audio Pack; no GLSL body" };
    }
    if (pack.kind == effects::Kind::Transition) {
        return { ShaderVerdict::Skipped,
                 "transition Pack; its entry is not exercised by the host "
                 "fragment path" };
    }
    if (!effects::is_expressible(pack)) {
        return { ShaderVerdict::Skipped, "not expressible: the pack draws named intermediates" };
    }

    // Resolve with nothing set, so the pack's own defaults fill every field -
    // the same defaults the engine would run a fresh instance with.
    const project::AppliedFilter instance = project::AppliedFilter::create(pack.id);
    std::optional<core::ShaderPass> pass = effects::resolve_pass(pack, instance, 0.0);
    if (!pass) {
        return { ShaderVerdict::Failed, "the pack does not resolve to a pass" };
    }

    const effects::BodyResult body = effects::read_body(dir);
    if (!body.body) {
        return { ShaderVerdict::Failed, body.problem };
    }
    pass->source = std::make_shared<const std::string>(std::move(*body.body));

    const std::optional<std::string> source = ges::fragment_source_for(*pass);
    if (!source) {
        return { ShaderVerdict::Failed, "the body declares a second sampler2D" };
    }

    const CompileResult compiled = compile_fragment(*source);
    if (!compiled.available) {
        return { ShaderVerdict::Skipped, "no headless GL context" };
    }
    if (!compiled.compiled) {
        return { ShaderVerdict::Failed, compiled.log };
    }
    return { ShaderVerdict::Compiled, { } };
}

// The flattened clip kind, spelled like the document's.
const char *clip_kind_name(render::ClipKind kind)
{
    switch (kind) {
    case render::ClipKind::Video:
        return "video";
    case render::ClipKind::Audio:
        return "audio";
    case render::ClipKind::Image:
        return "image";
    case render::ClipKind::Layer:
        return "layer";
    }
    return "video";
}

// The Pack kind, spelled like the manifest's.
const char *pack_kind_name(effects::Kind kind)
{
    switch (kind) {
    case effects::Kind::Effect:
        return "effect";
    case effects::Kind::Filter:
        return "filter";
    case effects::Kind::Audio:
        return "audio";
    case effects::Kind::Transition:
        return "transition";
    case effects::Kind::Generator:
        return "generator";
    }
    return "effect";
}

// Writes one failure, as text or a one-key JSON object when `json_mode`.
void write_error(std::ostream &out, const std::string &message, bool json_mode)
{
    if (json_mode) {
        out << json{ { "error", message } }.dump() << '\n';
    } else {
        out << "error: " << message << '\n';
    }
}

// A document that loaded, together with the raw JSON it came from: the summary
// reads the settings (name, frame) from the document, so both are carried.
struct Loaded
{
    json doc;
    project::Project project;
};

// Reads, parses and settles a project document. The version gate is checked
// here (mirroring Document::migrate) so a newer document is named rather than
// reported as merely unreadable. On failure `error` says why.
std::optional<Loaded> load_document(const std::string &path, std::string &error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot read " + path;
        return std::nullopt;
    }
    std::optional<json> doc = core::parse_document_json(in, error);
    if (!doc) {
        error = path + ": " + error;
        return std::nullopt;
    }
    if (!doc->is_object()) {
        error = path + ": not a JSON object";
        return std::nullopt;
    }
    const std::optional<int> version = project::document_version(*doc);
    if (version && *version > project::DOCUMENT_VERSION) {
        error = path + ": written by a newer build (version " + std::to_string(*version)
                + "; this build reads up to " + std::to_string(project::DOCUMENT_VERSION) + ")";
        return std::nullopt;
    }
    std::optional<project::Project> project = project::from_document(*doc);
    if (!project) {
        error = path + ": unreadable document (no usable timelines)";
        return std::nullopt;
    }
    return Loaded{ std::move(*doc), std::move(*project) };
}

// The document's version, defaulting to the oldest (1) when the field is
// absent, as the reader does.
int document_version_or_default(const json &doc)
{
    return project::document_version(doc).value_or(project::DOCUMENT_VERSION);
}

// The origin an Extension came from, spelled as the CLI option does.
const char *origin_name(extensions::Origin origin)
{
    switch (origin) {
    case extensions::Origin::Builtin:
        return "builtin";
    case extensions::Origin::Curated:
        return "curated";
    case extensions::Origin::Developer:
        return "developer";
    case extensions::Origin::User:
        return "user";
    }
    return "user";
}

std::optional<extensions::Origin> parse_origin(const std::string &name)
{
    if (name == "builtin") {
        return extensions::Origin::Builtin;
    }
    if (name == "curated") {
        return extensions::Origin::Curated;
    }
    if (name == "developer") {
        return extensions::Origin::Developer;
    }
    if (name == "user") {
        return extensions::Origin::User;
    }
    return std::nullopt;
}

// Prints one install outcome: the installed root, or every refusal reason.
// Returns EXIT_OK on success, EXIT_FAIL otherwise.
int report_install(std::ostream &out, const extensions::InstallResult &result,
                   const std::string &what)
{
    if (!result.ok()) {
        out << "error: " << what << '\n';
        for (const std::string &problem : result.problems) {
            out << "  " << problem << '\n';
        }
        return EXIT_FAIL;
    }
    out << "installed: " << result.installed->string() << '\n';
    return EXIT_OK;
}

// The CLI's catalog fetcher: `curl -fsSL <url>`, reading the body from stdout.
// Returns nullopt when curl cannot start, is killed, or exits non-zero (curl
// `-f` fails on a non-2xx response, `-s` silences the progress meter, `-L`
// follows redirects). The headless CLI has no HTTP client, so it documents the
// `curl` binary assumption: reaching a remote catalog needs curl on PATH; a
// local `file://` url works with no network.
extensions::CatalogFetcher cli_catalog_fetcher()
{
    return [](const std::string &url) -> std::optional<std::string> {
        const extensions::SubprocessResult run =
                extensions::run_subprocess({ "curl", "-fsSL", url });
        if (!run.started || !run.exited || run.exit_status != 0) {
            return std::nullopt;
        }
        return run.stdout_text;
    };
}

} // namespace

int cmd_inspect(std::ostream &out, const std::string &path, bool json_mode)
{
    std::string error;
    const std::optional<Loaded> loaded = load_document(path, error);
    if (!loaded) {
        write_error(out, error, json_mode);
        return EXIT_FAIL;
    }

    std::string name;
    if (const auto it = loaded->doc.find("name"); it != loaded->doc.end() && it->is_string()) {
        name = it->get<std::string>();
    }
    const project::Timeline &active = loaded->project.active();
    const std::uint32_t width = active.video.width;
    const std::uint32_t height = active.video.height;
    const std::int64_t rate_num = active.video.rate_num;
    const std::int64_t rate_den = active.video.rate_den;
    const int version = document_version_or_default(loaded->doc);

    if (json_mode) {
        json timelines = json::array();
        for (const project::Timeline &timeline : loaded->project.timelines) {
            json entry = json::object();
            entry["id"] = timeline.id;
            entry["name"] = timeline.name;
            entry["tracks"] = static_cast<std::uint64_t>(timeline.tracks.size());
            entry["clips"] = static_cast<std::uint64_t>(timeline.clips.size());
            entry["duration"] = timeline.duration().to_string();
            timelines.push_back(std::move(entry));
        }
        json result = json::object();
        result["name"] = name;
        result["version"] = version;
        result["frame"] = json::object();
        result["frame"]["width"] = width;
        result["frame"]["height"] = height;
        result["frame"]["rateNum"] = rate_num;
        result["frame"]["rateDen"] = rate_den;
        result["media"] = static_cast<std::uint64_t>(loaded->project.media.size());
        result["fonts"] = static_cast<std::uint64_t>(loaded->project.fonts.size());
        result["timelines"] = std::move(timelines);
        out << result.dump() << '\n';
        return EXIT_OK;
    }

    out << "name: " << (name.empty() ? "(untitled)" : name) << '\n';
    out << "version: " << version << '\n';
    out << "frame: " << width << 'x' << height << " @ " << rate_num << '/' << rate_den << '\n';
    out << "media: " << loaded->project.media.size() << '\n';
    out << "fonts: " << loaded->project.fonts.size() << '\n';
    out << "timelines: " << loaded->project.timelines.size() << '\n';
    for (const project::Timeline &timeline : loaded->project.timelines) {
        out << "  " << timeline.id << " \"" << timeline.name
            << "\": tracks=" << timeline.tracks.size() << " clips=" << timeline.clips.size()
            << " duration=" << timeline.duration().to_string() << '\n';
    }
    return EXIT_OK;
}

int cmd_validate(std::ostream &out, const std::string &path)
{
    std::string error;
    const std::optional<Loaded> loaded = load_document(path, error);
    if (!loaded) {
        out << "error: " << error << '\n';
        return EXIT_FAIL;
    }
    out << "ok " << path << ": version " << document_version_or_default(loaded->doc) << ", "
        << loaded->project.media.size() << " media, " << loaded->project.timelines.size()
        << " timeline(s), " << loaded->project.fonts.size() << " font(s)\n";
    return EXIT_OK;
}

int cmd_flatten(std::ostream &out, const std::string &path,
                const std::optional<std::string> &timeline_id)
{
    std::string error;
    const std::optional<Loaded> loaded = load_document(path, error);
    if (!loaded) {
        out << "error: " << error << '\n';
        return EXIT_FAIL;
    }

    std::optional<std::string_view> tid;
    if (timeline_id) {
        tid = std::string_view(*timeline_id);
    }
    const std::vector<render::ExportClip> clips = render::flatten_timeline(loaded->project, tid);

    out << "clips: " << clips.size() << '\n';
    std::size_t index = 1;
    for (const render::ExportClip &clip : clips) {
        out << "  " << index << ' ' << clip_kind_name(clip.kind)
            << " start=" << clip.start.to_string() << " duration=" << clip.duration.to_string()
            << " source=" << clip.path << '\n';
        ++index;
    }
    return EXIT_OK;
}

int cmd_render(std::ostream &out, const std::string &path, const std::string &output,
               const std::optional<std::string> &preset)
{
    std::string error;
    const std::optional<Loaded> loaded = load_document(path, error);
    if (!loaded) {
        out << "error: " << error << '\n';
        return EXIT_FAIL;
    }

    // The named preset, or the first default ("h264") when the caller named
    // none. An unknown name is refused by name rather than guessed.
    const std::string_view name = preset.has_value() ? std::string_view(*preset)
                                                     : render::default_export_presets().front();
    const std::optional<render::ExportPreset> chosen = render::export_preset(name);
    if (!chosen) {
        out << "error: unknown preset '" << name << "'; known presets:";
        for (const render::ExportPreset &known : render::export_presets()) {
            out << ' ' << known.name;
        }
        out << '\n';
        return EXIT_FAIL;
    }

    // The preset supplies the codec and sound-codec families; the project's
    // active timeline supplies the frame and rate (the profile override), so a
    // headless export writes at the source's own resolution rather than the
    // preset's 1080p host default.
    render::ExportSpec spec = render::spec_from(*chosen);
    spec.output = output;
    const project::Timeline &active = loaded->project.active();
    spec.width = active.video.width;
    spec.height = active.video.height;
    spec.rate_num = active.video.rate_num;
    spec.rate_den = active.video.rate_den;
    spec.clips = render::flatten_timeline(loaded->project, std::nullopt);

    const std::vector<std::string> problems = render::validate(spec);
    if (!problems.empty()) {
        out << "error: " << path << '\n';
        for (const std::string &problem : problems) {
            out << "  " << problem << '\n';
        }
        return EXIT_FAIL;
    }

    render::ExportRequest request;
    request.width = spec.width;
    request.height = spec.height;
    request.rate_num = spec.rate_num;
    request.rate_den = spec.rate_den;
    render::BuiltTimeline built = render::build_timeline(
            request, core::FrameRate(core::Rational(spec.rate_num, spec.rate_den)),
            std::span<const render::ExportClip>(spec.clips), { }, { }, { }, std::nullopt);

    // One line per whole ten percent, so a long export stays readable.
    int last_percent = -1;
    const auto progress = [&out, &last_percent](const engine::ExportProgress &report) {
        const int percent = static_cast<int>(report.fraction * 100.0);
        if (percent >= last_percent + 10) {
            last_percent = percent;
            out << "render: " << percent << "%\n";
        }
    };
    const auto cancelled = []() { return false; };

    // GStreamer/GES are only needed by this verb; initialize them lazily, once,
    // so every other verb never touches the engine.
    static std::once_flag gst_once;
    std::call_once(gst_once, [] {
        gst_init(nullptr, nullptr);
        ges_init();
    });

    const engine::ExportOutcome outcome = engine::export_timeline(spec, built, progress, cancelled);

    if (!outcome.ok) {
        out << "error: " << outcome.error << '\n';
        return EXIT_FAIL;
    }
    out << "rendered: " << output << " (" << spec.width << 'x' << spec.height << " @ "
        << spec.rate_num << '/' << spec.rate_den << ", " << spec.clips.size() << " clip(s), "
        << outcome.duration.as_double() << "s)\n";
    return EXIT_OK;
}

int cmd_packs(std::ostream &out, const std::string &path)
{
    const effects::LoadResult result = effects::load_pack_file(std::filesystem::path(path));
    if (!result.problems.empty()) {
        out << "error: " << path << '\n';
        for (const effects::LoadError &problem : result.problems) {
            out << "  " << problem.where << ": " << problem.what << '\n';
        }
        return EXIT_FAIL;
    }

    const effects::Pack &pack = *result.pack;
    out << "id: " << pack.id << '\n';
    out << "name: " << pack.name << '\n';
    out << "kind: " << pack_kind_name(pack.kind) << '\n';
    out << "version: " << pack.version << '\n';
    out << "parameters: " << pack.parameters.size() << '\n';
    for (const effects::Parameter &parameter : pack.parameters) {
        out << "  " << parameter.key << ' ' << effects::name(parameter.type) << " ["
            << parameter.min << ", " << parameter.max << "] default=" << parameter.default_value
            << '\n';
    }
    out << "passes: " << pack.passes.size() << '\n';
    for (const effects::Pass &pass : pack.passes) {
        out << "  " << pass.target << '\n';
    }
    return EXIT_OK;
}

int cmd_packs_trial(std::ostream &out, const std::string &dir, bool validate_only)
{
    out << "trial: " << dir << '\n';

    const effects::LoadResult result =
            effects::load_pack_file(std::filesystem::path(dir) / "effect.toml");
    if (!result.problems.empty()) {
        out << "error: " << dir << '\n';
        for (const effects::LoadError &problem : result.problems) {
            out << "  " << problem.where << ": " << problem.what << '\n';
        }
        return EXIT_FAIL;
    }

    const effects::Pack &pack = *result.pack;
    out << "  manifest: ok (id " << pack.id << ", version " << pack.version << ", kind "
        << pack_kind_name(pack.kind) << ", " << pack.parameters.size() << " parameter(s))\n";

    if (validate_only) {
        out << "  shader: skipped (--validate-only)\n";
        out << "validated, not trusted (shader not compiled): a trial only "
               "parses and compiles; it does not install or grant trust. To "
               "install, run `genesis-cli extensions install "
            << dir
            << " --origin curated` (the default origin `user` is refused by "
               "policy).\n";
        return EXIT_OK;
    }

    const auto started = std::chrono::steady_clock::now();
    const ShaderReport shader = trial_shader(pack, std::filesystem::path(dir));
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - started)
                                    .count();

    switch (shader.verdict) {
    case ShaderVerdict::Compiled:
        out << "  shader: compiled (" << elapsed_ms << " ms)\n";
        out << "validated, not trusted: a trial only parses and compiles; "
               "it does not install or grant trust. To install, run "
               "`genesis-cli extensions install "
            << dir
            << " --origin curated` (the default origin `user` is refused "
               "by policy).\n";
        return EXIT_OK;
    case ShaderVerdict::Failed:
        out << "  shader: failed to compile\n";
        out << "    " << shader.detail << '\n';
        out << "error: the shader does not compile\n";
        return EXIT_FAIL;
    case ShaderVerdict::Skipped:
        out << "  shader: skipped (" << shader.detail << ")\n";
        out << "validated, not trusted (shader not compiled): a trial only "
               "parses and compiles; it does not install or grant trust. "
               "To install, run `genesis-cli extensions install "
            << dir
            << " --origin curated` (the default origin `user` is refused "
               "by policy).\n";
        return EXIT_OK;
    }
    return EXIT_FAIL;
}

int cmd_commands(std::ostream &out)
{
    struct Verb
    {
        const char *name;
        const char *description;
    };
    const std::vector<Verb> verbs = {
        { "AddClip", "place a clip of media on a named track" },
        { "AddClipAtFirstFree", "place a clip on the first free track" },
        { "AddCutoutStroke", "paint a stroke on a clip's cutout" },
        { "AddFont", "register a font file for titles" },
        { "AddLayerClip", "place a layer: a look or effect over a span" },
        { "AddMedia", "import a file into the media bin" },
        { "AddTextClip", "place a title clip" },
        { "AddTimeline", "add a timeline" },
        { "AddTrack", "append a lane" },
        { "Batch", "several commands as one atomic edit" },
        { "ClearClipKey", "remove one key of a clip property" },
        { "ClearClipKeys", "remove every key of a clip property" },
        { "ClearEffectKey", "remove one key of an effect parameter" },
        { "ClearEffectKeys", "remove every key of an effect parameter" },
        { "DetachAudio", "pull a video clip's sound onto its own clip" },
        { "FillSlot", "swap a file into a template slot" },
        { "FreezeFrame", "insert a freeze frame" },
        { "MergeClips", "rejoin split pieces of one clip" },
        { "MoveClips", "reposition clips" },
        { "MoveTimeline", "reorder a timeline tab" },
        { "ReattachAudio", "undo a detach, restoring the clip's sound" },
        { "RemoveClips", "delete clips" },
        { "RemoveFont", "unregister a font family" },
        { "RemoveMedia", "remove a bin item and its clips" },
        { "RemoveTimeline", "delete a timeline" },
        { "RemoveTrack", "delete a lane and its clips" },
        { "RenameTimeline", "rename a timeline tab" },
        { "ReplaceClipMedia", "point a clip at another file" },
        { "SelectTimeline", "switch the active timeline" },
        { "SetClipCutout", "set or clear a clip's cutout" },
        { "SetClipKey", "put a key on a clip property" },
        { "SetClipSpeed", "change a clip's playback rate" },
        { "SetClipSpeedCurve", "set or clear a clip's speed curve" },
        { "SetClipTransform", "adjust a clip's placement" },
        { "SetEffectKey", "put a key on an effect parameter" },
        { "SetMediaColorRange", "set a media file's picture levels" },
        { "SetMediaPlaceholder", "mark media as a template slot" },
        { "SetTimelineVideo", "set a timeline's frame and rate" },
        { "SetTrackFlag", "flip a track's visibility or mute" },
        { "SplitClips", "cut clips at a playhead time" },
        { "TrimClip", "drag one edge of a clip" },
        { "UpdateClip", "patch one clip's properties" },
        { "UpdateMediaPath", "relink a media item's path" },
    };

    std::vector<Verb> sorted = verbs;
    std::sort(sorted.begin(), sorted.end(), [](const Verb &a, const Verb &b) {
        return std::string_view(a.name) < std::string_view(b.name);
    });
    for (const Verb &verb : sorted) {
        out << verb.name << "  " << verb.description << '\n';
    }
    return EXIT_OK;
}

std::string default_extensions_store()
{
    if (const char *env = std::getenv("GENESIS_EXTENSIONS_DIR"); env != nullptr && *env != '\0') {
        return env;
    }
    if (const char *xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && *xdg != '\0') {
        return (std::filesystem::path(xdg) / "genesis" / "extensions").string();
    }
    if (const char *home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return (std::filesystem::path(home) / ".local" / "share" / "genesis" / "extensions")
                .string();
    }
    return "genesis-extensions";
}

int cmd_extensions_list(std::ostream &out, const std::string &store)
{
    const std::map<std::string, std::map<std::string, std::filesystem::path>> packs =
            extensions::installed_packs(store);
    const extensions::Graph graph = extensions::read_dependency_graph(store);
    out << "store: " << store << '\n';
    out << "packs: " << packs.size() << '\n';
    for (const auto &[id, versions] : packs) {
        out << "  " << id << '\n';
        for (const auto &[version, root] : versions) {
            out << "    " << version << ' ' << root.string() << '\n';
        }
        // The dependency edges for this id, from the native-extension manifests
        // only: a pack-only store prints no such lines.
        const auto deps = graph.requires_of.find(id);
        if (deps != graph.requires_of.end() && !deps->second.empty()) {
            out << "    requires:";
            for (const std::string &required : deps->second) {
                out << ' ' << required;
            }
            out << '\n';
        }
        const auto dependents = graph.dependents.find(id);
        if (dependents != graph.dependents.end() && !dependents->second.empty()) {
            out << "    dependents:";
            for (const std::string &dependent : dependents->second) {
                out << ' ' << dependent;
            }
            out << '\n';
        }
    }
    return EXIT_OK;
}

int cmd_extensions_install(std::ostream &out, const std::string &source, const std::string &store,
                           const std::string &origin)
{
    const std::optional<extensions::Origin> parsed = parse_origin(origin);
    if (!parsed) {
        out << "error: unknown origin '" << origin
            << "'; use builtin, curated, developer or user\n";
        return EXIT_FAIL;
    }
    const extensions::InstallResult result = extensions::install_from_source(
            extensions::InstallSource{ .type = extensions::SourceType::Dir, .url = source }, store,
            *parsed);
    return report_install(out, result, source);
}

int cmd_extensions_install_git(std::ostream &out, const std::string &url,
                               const std::optional<std::string> &ref,
                               const std::optional<std::string> &subdir, const std::string &store,
                               const std::string &origin)
{
    const std::optional<extensions::Origin> parsed = parse_origin(origin);
    if (!parsed) {
        out << "error: unknown origin '" << origin
            << "'; use builtin, curated, developer or user\n";
        return EXIT_FAIL;
    }
    extensions::InstallSource source;
    source.type = extensions::SourceType::Git;
    source.url = url;
    if (ref) {
        source.ref = *ref;
    }
    if (subdir) {
        source.subdir = *subdir;
    }
    const extensions::InstallResult result =
            extensions::install_from_source(source, store, *parsed);
    return report_install(out, result, url);
}

int cmd_extensions_catalog(std::ostream &out, const std::string &url)
{
    const extensions::CatalogResult catalog = extensions::fetch_catalog(url, cli_catalog_fetcher());
    if (!catalog.entries) {
        out << "error: " << url << '\n';
        for (const std::string &problem : catalog.problems) {
            out << "  " << problem << '\n';
        }
        return EXIT_FAIL;
    }
    out << "catalog: " << url << '\n';
    out << "entries: " << catalog.entries->size() << '\n';
    for (const extensions::CatalogEntry &entry : *catalog.entries) {
        out << "  " << entry.id << ' ' << entry.version << " (" << entry.name
            << ") source=" << extensions::source_type_name(entry.source.type) << ':'
            << entry.source.url;
        if (!entry.source.ref.empty()) {
            out << " ref=" << entry.source.ref;
        }
        if (!entry.source.subdir.empty()) {
            out << " subdir=" << entry.source.subdir;
        }
        out << '\n';
    }
    return EXIT_OK;
}

int cmd_extensions_install_catalog(std::ostream &out, const std::string &url, const std::string &id,
                                   const std::string &store, const std::string &origin)
{
    const std::optional<extensions::Origin> parsed = parse_origin(origin);
    if (!parsed) {
        out << "error: unknown origin '" << origin
            << "'; use builtin, curated, developer or user\n";
        return EXIT_FAIL;
    }
    const extensions::InstallResult result =
            extensions::install_from_catalog(url, id, store, *parsed, cli_catalog_fetcher());
    return report_install(out, result, id);
}

int cmd_extensions_trust(std::ostream &out, const std::optional<std::string> &origin)
{
    const effects::Pack pack; // permissions_for does not read it today
    const auto report = [&](extensions::Origin value) {
        const extensions::Permission permission = extensions::permissions_for(value, pack);
        out << origin_name(value) << ": "
            << (extensions::is_trusted(value) ? "trusted" : "not trusted")
            << " (execute: " << (permission.execute ? "yes" : "no")
            << ", filesystem: " << (permission.filesystem ? "yes" : "no")
            << ", network: " << (permission.network ? "yes" : "no") << ")\n";
    };
    if (origin) {
        const std::optional<extensions::Origin> parsed = parse_origin(*origin);
        if (!parsed) {
            out << "error: unknown origin '" << *origin
                << "'; use builtin, curated, developer or user\n";
            return EXIT_FAIL;
        }
        report(*parsed);
        return EXIT_OK;
    }
    report(extensions::Origin::Builtin);
    report(extensions::Origin::Curated);
    report(extensions::Origin::Developer);
    report(extensions::Origin::User);
    return EXIT_OK;
}

int cmd_extensions_remove(std::ostream &out, const std::string &store, const std::string &id)
{
    // Tombstone first: if the record cannot be written, the copy must stay, or
    // the next seed would silently re-add a builtin the user meant to remove.
    if (!extensions::remove(store, id)) {
        out << "error: cannot record the removal of '" << id << "'\n";
        return EXIT_FAIL;
    }
    // The installed copy may already be absent (never installed, or removed
    // before); remove_all treats a missing path as a successful no-op.
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(store) / id, ec);
    if (ec) {
        out << "error: cannot remove the installed copy of '" << id << "': " << ec.message()
            << '\n';
        return EXIT_FAIL;
    }
    // Removing an extension leaves its dependents pointing at a missing id, so
    // they are disabled (with `requires <id>` recorded) rather than left to
    // fail at load.
    extensions::Manager manager(store);
    manager.propagate_removed(id);
    out << "removed: " << id << '\n';
    return EXIT_OK;
}

int cmd_extensions_restore(std::ostream &out, const std::string &store, const std::string &id)
{
    if (!extensions::restore(store, id)) {
        out << "error: cannot clear the removal of '" << id << "'\n";
        return EXIT_FAIL;
    }
    // Re-seed the builtins: the restored id (a builtin) installs again, and any
    // other not-tombstoned missing builtin does too. The seed is idempotent and
    // skips tombstoned ids, so this only adds back what the user asked for.
    const std::vector<std::string> problems =
            extensions::seed(std::filesystem::path(store), extensions::builtin_root());
    for (const std::string &problem : problems) {
        out << "note: " << problem << '\n';
    }
    out << "restored: " << id << '\n';
    return EXIT_OK;
}

int cmd_extensions_enable(std::ostream &out, const std::string &store, const std::string &id)
{
    extensions::Manager manager(store);
    if (!manager.enable(id)) {
        out << "error: cannot enable '" << id << "'\n";
        return EXIT_FAIL;
    }
    out << "enabled: " << id << '\n';
    return EXIT_OK;
}

int cmd_extensions_disable(std::ostream &out, const std::string &store, const std::string &id)
{
    extensions::Manager manager(store);
    if (!manager.disable(id)) {
        out << "error: cannot disable '" << id << "'\n";
        return EXIT_FAIL;
    }
    out << "disabled: " << id << '\n';
    return EXIT_OK;
}

int cmd_config_export(std::ostream &out, const std::string &file, const std::string &store)
{
    // No settings seam: the headless CLI has no app controllers to read, so the
    // exported profile's `settings` object is empty (every setting is omitted).
    const extensions::ConfigSeam seam;
    const nlohmann::json profile = extensions::export_profile(store, seam, kCliVersion);

    std::ofstream fout(file);
    if (!fout) {
        out << "error: cannot write " << file << '\n';
        return EXIT_FAIL;
    }
    fout << profile.dump(2) << '\n';
    if (!fout) {
        out << "error: cannot write " << file << '\n';
        return EXIT_FAIL;
    }
    out << "exported: " << file << '\n';
    return EXIT_OK;
}

int cmd_config_import(std::ostream &out, const std::string &file, const std::string &store,
                      bool no_install)
{
    std::ifstream in(file);
    if (!in) {
        out << "error: cannot read " << file << '\n';
        return EXIT_FAIL;
    }
    nlohmann::json profile;
    try {
        in >> profile;
    } catch (const nlohmann::json::exception &) {
        out << "error: " << file << " is not valid JSON\n";
        return EXIT_FAIL;
    }

    // --no-install drops the extensions so only the settings are applied (none,
    // in this headless CLI); the note below still says what was skipped.
    if (no_install) {
        profile.erase("extensions");
        out << "extensions skipped (--no-install)\n";
    }

    // No settings seam: the headless CLI has no app controllers to set, so every
    // profile setting is reported as skipped rather than applied.
    const extensions::ConfigSeam seam;
    const std::optional<extensions::ImportReport> report =
            extensions::import_profile(profile, store, seam);
    if (!report) {
        out << "error: " << file << " is not a readable config profile\n";
        return EXIT_FAIL;
    }

    out << "settings:\n";
    for (const std::string &line : report->settings) {
        out << "  " << line << '\n';
    }
    out << "extensions:\n";
    for (const extensions::ImportedExtension &entry : report->extensions) {
        out << "  " << entry.status << ": " << entry.id;
        if (!entry.reason.empty()) {
            out << " (" << entry.reason << ")";
        }
        out << '\n';
    }
    return EXIT_OK;
}

// The API services the CLI wires: the one extension store (the default store)
// through its Manager seam, and the config profile through a settings-less
// seam. Media and catalogue stay unwired in this headless host, so those
// requests are refused by dispatch with a typed error.
api::Services cli_services()
{
    api::Services services;
    services.extensions =
            api::extensions_seam(std::make_shared<extensions::Manager>(default_extensions_store()));
    const std::string store = default_extensions_store();
    const extensions::ConfigSeam seam; // no settings to read or apply
    services.config_export = api::config_export_seam(store, seam, kCliVersion);
    services.config_import = api::config_import_seam(store, seam);
    services.extensions_catalog = api::extensions_catalog_seam(cli_catalog_fetcher());
    return services;
}

namespace {
// Reads one newline-terminated line from `in`, bounded to the request cap.
// Returns false only at end-of-input with no bytes read. A line longer than
// the cap is truncated to the cap and `oversized` is set, so the caller can
// answer with the oversize reply instead of feeding the parser a partial line.
bool read_line(std::istream &in, std::string &line, bool &oversized)
{
    line.clear();
    oversized = false;
    bool any = false;
    int c = in.get();
    while (c != std::char_traits<char>::eof()) {
        any = true;
        if (c == '\n') {
            return true;
        }
        if (line.size() < kMaxJsonRequestBytes) {
            line.push_back(static_cast<char>(c));
        } else {
            oversized = true;
        }
        c = in.get();
    }
    return any;
}
} // namespace

int cmd_api(std::istream &in, std::ostream &out)
{
    project::Editor editor;
    // The CLI links the engine only for `render`; the probe and catalogue
    // services stay unwired here, so media and catalogue requests are refused
    // by dispatch with a typed error rather than guessed.
    const api::Services services = cli_services();

    bool shutdown = false;
    std::string line;
    bool oversized = false;
    while (!shutdown && read_line(in, line, oversized)) {
        if (oversized) {
            out << oversize_request_reply() << '\n';
            continue;
        }
        if (const std::optional<std::string> reply =
                    handle_jsonrpc_line(editor, services, line, shutdown)) {
            out << *reply << '\n';
        }
    }
    return EXIT_OK;
}

} // namespace genesis::cli
