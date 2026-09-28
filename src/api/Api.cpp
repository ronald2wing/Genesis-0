// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "api/Api.h"

#include <fstream>
#include <memory>
#include <string>
#include <utility>

#include "core/JsonGuard.h"
#include "extensions/Catalog.h"
#include "extensions/Config.h"
#include "extensions/ExtensionHost.h"
#include "extensions/Manager.h"
#include "extensions/Origins.h"
#include "project/Document.h"
#include "project/Editor.h"
#include "render/ExportPresets.h"
#include "render/TitleTemplates.h"
#include "workspace/AssetPaths.h"
#include "workspace/ProjectClaim.h"

namespace genesis::api {

namespace {
namespace project = genesis::project;
namespace core = genesis::core;
namespace render = genesis::render;
namespace workspace = genesis::workspace;
using project::Project;

Response ok(Reply reply)
{
    return Response{ std::move(reply) };
}
Response fail(ApiError error)
{
    return Response{ std::unexpected(std::move(error)) };
}

// A method whose feature is not built in this engine-free host. The request's
// shape is carried (see Requests.h) but there is nothing to run it against, so
// the caller gets a typed refusal rather than a silent success.
ApiError not_available(std::string feature)
{
    return ApiError{ ErrorCode::NotAvailable, feature + " is not built yet" };
}

// The session's settings as the window shows them: the active timeline's frame
// and rate. The name is not part of the Project model - it lives in a settings
// store that is a later concern - so it reads empty here.
SettingsView settings_of(const Project &project)
{
    const auto &video = project.active().video;
    return SettingsView{ "", video.width, video.height, video.rate_num, video.rate_den };
}

// The authoritative state plus history availability, without `created_id`.
EditorView view_of(const Project &project, bool can_undo, bool can_redo)
{
    return EditorView{ project, can_undo, can_redo, settings_of(project), std::nullopt };
}

// The state of an open editor, carrying the id a creating command minted.
EditorView view_of(const project::Editor &editor, std::optional<std::string> created_id)
{
    return EditorView{ editor.project(), editor.can_undo(), editor.can_redo(),
                       settings_of(editor.project()), std::move(created_id) };
}

// Writes `doc` to `path`. Returns nullopt on success, or why it failed.
std::optional<std::string> write_document(const std::string &path, const nlohmann::json &doc)
{
    std::ofstream out(path);
    if (!out) {
        return "could not write " + path;
    }
    out << doc.dump(2);
    if (!out) {
        return "could not write " + path;
    }
    return std::nullopt;
}

// Reads a project document from `path`. Returns the loaded project, or why it
// could not be read: a newer build wrote it, or it holds a document this build
// cannot read.
std::expected<Project, std::string> read_document(const std::string &path)
{
    std::ifstream in(path);
    if (!in) {
        return std::unexpected("could not read " + path);
    }
    std::string error;
    std::optional<nlohmann::json> parsed = core::parse_document_json(in, error);
    if (!parsed) {
        return std::unexpected(path + ": " + error);
    }
    const nlohmann::json &doc = *parsed;
    if (const std::optional<int> version = project::document_version(doc);
        version && *version > project::DOCUMENT_VERSION) {
        return std::unexpected(path + " was saved by a newer build than this one");
    }
    if (std::optional<Project> loaded = project::from_document(doc)) {
        return std::move(*loaded);
    }
    return std::unexpected(path + " holds a document this build cannot read");
}

// The MediaSummary a probe's `NewMedia` becomes. A summary, not an
// exhaustive transcription: the host model keeps more than the API reports.
MediaSummary media_summary(const project::NewMedia &item)
{
    MediaSummary summary;
    summary.path = item.path;
    if (item.duration) {
        summary.duration = item.duration->as_double();
    }
    summary.kind = item.kind;
    if (item.width && item.height) {
        VideoStreamInfo video;
        video.index = 0;
        video.codec = item.video_codec.value_or("");
        video.width = *item.width;
        video.height = *item.height;
        video.frame_rate = item.frame_rate.value_or(0.0);
        video.frame_rate_fraction = item.frame_rate_fraction.value_or("");
        video.color_space = item.color_space;
        summary.video = std::move(video);
    }
    const auto track_info = [](const project::AudioTrack &track) {
        AudioStreamInfo audio;
        audio.index = track.index;
        audio.codec = track.codec;
        audio.sample_rate = track.sample_rate;
        audio.channels = track.channels;
        audio.title = track.title;
        audio.language = track.language;
        return audio;
    };
    if (!item.audio_tracks.empty()) {
        summary.audio = track_info(item.audio_tracks.front());
        summary.audio_tracks.reserve(item.audio_tracks.size());
        for (const project::AudioTrack &track : item.audio_tracks) {
            summary.audio_tracks.push_back(track_info(track));
        }
    }
    return summary;
}

// The manifest spelling of a media kind, the reverse of the document's
// `read_media_kind`.
std::string_view media_kind_name(project::MediaKind kind)
{
    switch (kind) {
    case project::MediaKind::Video:
        return "video";
    case project::MediaKind::Audio:
        return "audio";
    case project::MediaKind::Image:
        return "image";
    }
    return "video";
}

// The MediaListView a bin of MediaItems becomes, for `media.list`.
MediaListView media_list_view(const std::vector<project::MediaItem> &items)
{
    MediaListView view;
    view.media.reserve(items.size());
    for (const project::MediaItem &item : items) {
        MediaListEntry entry;
        entry.id = item.id;
        entry.path = item.path;
        entry.name = item.name;
        entry.kind = std::string(media_kind_name(item.kind));
        if (item.duration) {
            entry.duration = item.duration->as_double();
        }
        entry.width = item.width;
        entry.height = item.height;
        view.media.push_back(std::move(entry));
    }
    return view;
}

// The PackageInfo a catalogue Pack becomes.
PackageInfo package_info(const effects::Pack &pack)
{
    PackageInfo info;
    info.id = pack.id;
    info.name = pack.name;
    info.kind = std::string(effects::kind_name(pack.kind));
    info.category = pack.category;
    info.description = pack.description;
    info.intensity = pack.intensity;
    info.params.reserve(pack.parameters.size());
    for (const effects::Parameter &parameter : pack.parameters) {
        ParamInfo param;
        param.key = parameter.key;
        param.label = parameter.label;
        param.kind = std::string(effects::name(parameter.type));
        param.min = parameter.min;
        param.max = parameter.max;
        param.default_value = parameter.default_value;
        param.step = parameter.step;
        param.unit = parameter.unit;
        param.animate = parameter.animate;
        param.values = parameter.values;
        param.labels = parameter.labels;
        info.params.push_back(std::move(param));
    }
    return info;
}

// Applies one command to the editor, turning a command-layer refusal into an
// `ApiError::Refused` carrying the sentence the window would show.
Response apply_or_refuse(project::Editor &editor, const project::Command &command,
                         bool with_created_id)
{
    const project::ApplyResult result = editor.apply(command);
    if (!result) {
        return fail(ApiError{ ErrorCode::Refused, std::string(result.error().message()) });
    }
    return ok(Reply{ view_of(editor, with_created_id ? result->created_id : std::nullopt) });
}

// The load state of an extension record, as the wire spells it.
std::string_view state_name(extensions::ExtensionState state)
{
    switch (state) {
    case extensions::ExtensionState::Enabled:
        return "enabled";
    case extensions::ExtensionState::Disabled:
        return "disabled";
    case extensions::ExtensionState::Revoked:
        return "revoked";
    case extensions::ExtensionState::Failed:
        return "failed";
    }
    return "failed";
}

// The document-layer spelling of a title alignment (`read_text_align_strict`'s
// reverse), shared by the template replies so their style JSON is the exact
// shape `edit.apply`'s `addTextClip` consumes.
std::string_view text_align_name(project::TextAlign align)
{
    switch (align) {
    case project::TextAlign::Left:
        return "left";
    case project::TextAlign::Center:
        return "center";
    case project::TextAlign::Right:
        return "right";
    }
    return "center";
}

// A `TextStyle` as the document/command layer spells it (camelCase keys). This
// mirrors `project/document/Write.cpp`'s `write_text_style` field for field, so
// a filled slot's style is byte-for-byte what `edit.apply` accepts.
nlohmann::json text_style_json(const project::TextStyle &style)
{
    nlohmann::json j = nlohmann::json::object();
    j["content"] = style.content;
    j["fontFamily"] = style.font_family;
    j["fontSize"] = style.font_size;
    j["fontWeight"] = style.font_weight;
    j["italic"] = style.italic;
    j["color"] = style.color;
    j["align"] = std::string(text_align_name(style.align));
    j["opacity"] = style.opacity;
    j["strokeWidth"] = style.stroke_width;
    j["strokeColor"] = style.stroke_color;
    j["shadow"] = style.shadow;
    j["background"] = style.background;
    j["backgroundRadius"] = style.background_radius;
    j["backgroundPaddingX"] = style.background_padding_x;
    j["backgroundPaddingY"] = style.background_padding_y;
    j["lineHeight"] = style.line_height;
    j["tracking"] = style.tracking;
    j["maxWidth"] = style.max_width;
    j["maxHeight"] = style.max_height;
    return j;
}

// The in-house title-template library, discovered on demand from the asset
// root's `titles/` directory. Cheap enough (20 files) to read per call; the
// app's own gallery re-discovers the same way rather than caching.
render::TemplateLibrary title_library()
{
    return render::discover_templates(workspace::asset_root() / "titles");
}

// `template.list`'s reply: one object per template - its name and its text
// slots, each slot carrying the placeholder and the styling the host can
// express. The placeholder doubles as the slot's default content, so a listed
// style is already a complete `addTextClip` style when taken verbatim.
nlohmann::json title_template_list_json()
{
    nlohmann::json out = nlohmann::json::array();
    for (const render::TitleTemplate &tmpl : title_library().templates) {
        nlohmann::json slots = nlohmann::json::array();
        for (const render::TitleSlot &slot : tmpl.text_slots) {
            project::TextStyle style = slot.style;
            style.content = slot.placeholder;
            slots.push_back(nlohmann::json{ { "placeholder", slot.placeholder },
                                            { "style", text_style_json(style) } });
        }
        out.push_back(nlohmann::json{ { "name", tmpl.name }, { "slots", std::move(slots) } });
    }
    return out;
}

} // namespace

ExtensionInfo extension_info_from(const extensions::ExtensionRecord &record)
{
    ExtensionInfo info;
    info.id = record.id;
    info.name = record.name;
    info.version = record.version;
    info.origin = extensions::origin_name(record.origin);
    info.state = std::string(state_name(record.state));
    info.reason = record.reason;
    info.dependencies = record.dependencies;
    info.dependents = record.dependents;
    return info;
}

Response dispatch_extensions(const ExtensionsRequest &request, extensions::Manager &manager)
{
    return std::visit(
            [&](const auto &alternative) -> Response {
                using T = std::decay_t<decltype(alternative)>;
                if constexpr (std::is_same_v<T, ExtensionsList>) {
                    ExtensionsInstalled installed;
                    for (const extensions::ExtensionRecord &record : manager.list_extensions()) {
                        installed.extensions.push_back(extension_info_from(record));
                    }
                    return ok(Reply{ std::move(installed) });
                } else if constexpr (std::is_same_v<T, ExtensionsRemove>) {
                    if (!manager.remove_extension(alternative.id)) {
                        return fail(ApiError{ ErrorCode::Refused,
                                              "cannot remove '" + alternative.id + "'" });
                    }
                    return ok(Reply{ Done{ } });
                } else if constexpr (std::is_same_v<T, ExtensionsEnable>) {
                    manager.enable(alternative.id);
                    return ok(Reply{ Done{ } });
                } else if constexpr (std::is_same_v<T, ExtensionsDisable>) {
                    manager.disable(alternative.id);
                    return ok(Reply{ Done{ } });
                } else if constexpr (std::is_same_v<T, ExtensionsRestore>) {
                    if (!manager.restore_extension(alternative.id)) {
                        return fail(ApiError{ ErrorCode::Refused,
                                              "cannot restore '" + alternative.id + "'" });
                    }
                    return ok(Reply{ Done{ } });
                } else if constexpr (std::is_same_v<T, ExtensionsInstall>) {
                    // Absent reads as Developer (consent-gated), the same default
                    // the app's install invokable and a config import use.
                    const extensions::Origin origin = alternative.origin
                            ? extensions::parse_origin(*alternative.origin)
                            : extensions::Origin::Developer;
                    const extensions::InstallResult result =
                            manager.install_from_source(alternative.source, origin);
                    if (!result.ok()) {
                        // The first problem is the actionable refusal; the rest are
                        // its detail. A refused install wrote nothing to the store.
                        const std::string reason = result.problems.empty()
                                ? "install refused"
                                : result.problems.front();
                        return fail(ApiError{ ErrorCode::Refused, std::move(reason) });
                    }
                    return ok(
                            Reply{ nlohmann::json{ { "installed", result.installed->string() } } });
                } else {
                    static_assert(detail::is_extensions_request_v<T>,
                                  "extension request has no handler");
                }
            },
            request.value);
}

std::function<Response(const ExtensionsRequest &)>
extensions_seam(std::shared_ptr<extensions::Manager> manager)
{
    return [manager = std::move(manager)](const ExtensionsRequest &request) {
        return dispatch_extensions(request, *manager);
    };
}

// The ImportReport an extensions::import_profile returns, as the wire reply.
ConfigImportView config_import_view(const extensions::ImportReport &report)
{
    ConfigImportView view;
    view.settings = report.settings;
    view.extensions.reserve(report.extensions.size());
    for (const extensions::ImportedExtension &entry : report.extensions) {
        view.extensions.push_back(ConfigImportEntry{ entry.id, entry.status, entry.reason });
    }
    return view;
}

std::function<Response(const ConfigExport &)> config_export_seam(std::filesystem::path store_root,
                                                                 extensions::ConfigSeam seam,
                                                                 std::string app_version)
{
    return [store_root = std::move(store_root), seam = std::move(seam),
            app_version = std::move(app_version)](const ConfigExport &) {
        return ok(Reply{ extensions::export_profile(store_root, seam, app_version) });
    };
}

std::function<Response(const ConfigImport &)> config_import_seam(std::filesystem::path store_root,
                                                                 extensions::ConfigSeam seam)
{
    return [store_root = std::move(store_root),
            seam = std::move(seam)](const ConfigImport &request) {
        const std::optional<extensions::ImportReport> report =
                extensions::import_profile(request.profile, store_root, seam);
        if (!report) {
            return fail(
                    ApiError{ ErrorCode::Failed, "the profile is not a readable config profile" });
        }
        return ok(Reply{ config_import_view(*report) });
    };
}

std::function<Response(const ExtensionsCatalog &)>
extensions_catalog_seam(std::function<std::optional<std::string>(const std::string &)> fetcher)
{
    return [fetcher = std::move(fetcher)](const ExtensionsCatalog &request) {
        if (!fetcher) {
            return fail(ApiError{ ErrorCode::NotAvailable, "the catalog fetcher is not wired" });
        }
        const extensions::CatalogResult catalog = extensions::fetch_catalog(request.url, fetcher);
        if (!catalog.entries) {
            std::string reason;
            for (const std::string &problem : catalog.problems) {
                if (!reason.empty()) {
                    reason += "; ";
                }
                reason += problem;
            }
            return fail(ApiError{ ErrorCode::Failed, std::move(reason) });
        }
        return ok(Reply{ extensions::catalog_entries_json(*catalog.entries) });
    };
}

std::vector<ExportPresetInfo> export_presets_view()
{
    std::vector<ExportPresetInfo> presets;
    for (const render::ExportPreset &preset : render::export_presets()) {
        ExportPresetInfo info;
        info.name = std::string(preset.name);
        info.width = preset.width;
        info.height = preset.height;
        info.rate_num = preset.rate_num;
        info.rate_den = preset.rate_den;
        info.fps = static_cast<double>(preset.rate_num) / static_cast<double>(preset.rate_den);
        info.codec = std::string(render::codec_name(preset.codec));
        info.quality = std::string(preset.quality);
        presets.push_back(std::move(info));
    }
    return presets;
}

std::string ai_status_state(std::string_view controller_state, std::string_view stage)
{
    if (controller_state == "Running") {
        // The download phase is the one part of a run `ai.status` distinguishes,
        // so a caller can tell "fetching weights" from "computing".
        return stage == "downloading model" ? "downloading" : "running";
    }
    if (controller_state == "Done") {
        return "done";
    }
    if (controller_state == "Failed" || controller_state == "Cancelled") {
        return "failed";
    }
    // "Idle" and "Consent" (waiting for a human to approve a download) both
    // read idle: the API implies consent, so a prompt never surfaces.
    return "idle";
}

Response dispatch(const Request &request, project::Editor &editor, const Services &services)
{
    return std::visit(
            [&](const auto &alternative) -> Response {
                using T = std::decay_t<decltype(alternative)>;
                if constexpr (std::is_same_v<T, Version>) {
                    VersionInfo info;
                    info.api_version = std::string(API_VERSION);
                    info.dirs = Dirs{ };
                    // Engine-free, transport-free: no events, gpu, or socket
                    // transports to advertise. Capabilities grow as those land.
                    info.capabilities = { };
                    return ok(Reply{ std::move(info) });
                } else if constexpr (std::is_same_v<T, ProjectCreate>) {
                    // `location` and `name` are host-level settings the
                    // single-Editor host does not keep yet (folder creation and
                    // recents live in the session registry); the frame and rate
                    // land in the fresh project's one timeline.
                    const Project project = alternative.video
                            ? Project::with_video(*alternative.video)
                            : Project{ };
                    return ok(Reply{ view_of(project, false, false) });
                } else if constexpr (std::is_same_v<T, ProjectOpen>) {
                    // The Rust host opens a project folder and finds the document
                    // inside it; this single-Editor host treats `path` as the
                    // document file itself, since the session registry that maps a
                    // folder to its Editor and document file is a later concern.
                    std::expected<Project, std::string> loaded = read_document(alternative.path);
                    if (!loaded) {
                        return fail(ApiError{ ErrorCode::Failed, std::move(loaded.error()) });
                    }
                    // Claim the folder the document lives in (a standalone
                    // document claims nothing), so a second process is refused
                    // with a precise reason before this one starts editing.
                    if (std::optional<std::string> refusal =
                                workspace::claim_project_path(alternative.path)) {
                        return fail(ApiError{ ErrorCode::Refused, std::move(*refusal) });
                    }
                    return ok(Reply{ view_of(*loaded, false, false) });
                } else if constexpr (std::is_same_v<T, ProjectClose>) {
                    if (alternative.save) {
                        if (std::optional<std::string> problem = write_document(
                                    alternative.path, project::to_document(editor.project()))) {
                            return fail(ApiError{ ErrorCode::Failed, std::move(*problem) });
                        }
                        editor.mark_saved();
                    }
                    workspace::release_project();
                    return ok(Reply{ Done{ } });
                } else if constexpr (std::is_same_v<T, ProjectGet>) {
                    return ok(Reply{ view_of(editor, std::nullopt) });
                } else if constexpr (std::is_same_v<T, ProjectDocument>) {
                    return ok(Reply{ project::to_document(editor.project()) });
                } else if constexpr (std::is_same_v<T, ProjectSave>) {
                    // `name` renames the project; the name is not on the Project
                    // model, so it is carried but not stored yet.
                    if (std::optional<std::string> problem = write_document(
                                alternative.path, project::to_document(editor.project()))) {
                        return fail(ApiError{ ErrorCode::Failed, std::move(*problem) });
                    }
                    editor.mark_saved();
                    return ok(Reply{ Done{ } });
                } else if constexpr (std::is_same_v<T, ProjectSetVideo>) {
                    const std::string timeline_id = editor.project().active_timeline_id;
                    return apply_or_refuse(editor,
                                           project::Command{ project::SetTimelineVideo{
                                                   timeline_id, alternative.video } },
                                           false);
                } else if constexpr (std::is_same_v<T, EditApply>) {
                    return apply_or_refuse(editor, alternative.command, true);
                } else if constexpr (std::is_same_v<T, EditUndo>) {
                    editor.undo();
                    return ok(Reply{ view_of(editor, std::nullopt) });
                } else if constexpr (std::is_same_v<T, EditRedo>) {
                    editor.redo();
                    return ok(Reply{ view_of(editor, std::nullopt) });
                } else if constexpr (std::is_same_v<T, MediaProbe>) {
                    if (!services.probe) {
                        return fail(ApiError{ ErrorCode::Failed, "no probe service is wired" });
                    }
                    std::optional<project::NewMedia> probed = services.probe(alternative.path);
                    if (!probed) {
                        return fail(ApiError{ ErrorCode::Failed,
                                              "could not probe " + alternative.path });
                    }
                    return ok(Reply{ media_summary(*probed) });
                } else if constexpr (std::is_same_v<T, MediaImport>) {
                    if (!services.probe) {
                        return fail(ApiError{ ErrorCode::Failed, "no probe service is wired" });
                    }
                    std::optional<project::NewMedia> probed = services.probe(alternative.file);
                    if (!probed) {
                        return fail(ApiError{ ErrorCode::Failed,
                                              "could not probe " + alternative.file });
                    }
                    return apply_or_refuse(
                            editor, project::Command{ project::AddMedia{ std::move(*probed) } },
                            true);
                } else if constexpr (std::is_same_v<T, CatalogueList>) {
                    if (!services.catalogue) {
                        return fail(ApiError{ ErrorCode::Failed, "no catalogue service is wired" });
                    }
                    std::vector<PackageInfo> packages;
                    for (const effects::Pack &pack : services.catalogue(alternative.kind)) {
                        packages.push_back(package_info(pack));
                    }
                    return ok(Reply{ std::move(packages) });
                } else if constexpr (std::is_same_v<T, TemplateList>) {
                    return ok(Reply{ title_template_list_json() });
                } else if constexpr (std::is_same_v<T, TemplateFill>) {
                    const render::TemplateLibrary library = title_library();
                    const render::TitleTemplate *found = nullptr;
                    for (const render::TitleTemplate &tmpl : library.templates) {
                        if (tmpl.name == alternative.template_) {
                            found = &tmpl;
                            break;
                        }
                    }
                    if (!found) {
                        return fail(ApiError{ ErrorCode::NotFound,
                                              "unknown template '" + alternative.template_ + "'" });
                    }
                    const auto filled = render::fill_slots(*found, alternative.texts);
                    nlohmann::json slots = nlohmann::json::array();
                    for (const auto &[text, style] : filled) {
                        project::TextStyle styled = style;
                        styled.content = text;
                        slots.push_back(nlohmann::json{ { "text", text },
                                                        { "style", text_style_json(styled) } });
                    }
                    return ok(Reply{ std::move(slots) });
                } else if constexpr (std::is_same_v<T, ExportRun>) {
                    if (!services.export_run) {
                        return fail(not_available("the export runner"));
                    }
                    return services.export_run(alternative);
                } else if constexpr (std::is_same_v<T, ExportCancel>) {
                    if (!services.export_cancel) {
                        return fail(not_available("the export runner"));
                    }
                    return services.export_cancel(alternative);
                } else if constexpr (std::is_same_v<T, ExportStatus>) {
                    if (!services.export_status) {
                        return fail(not_available("the export runner"));
                    }
                    return services.export_status(alternative);
                } else if constexpr (std::is_same_v<T, ExportPresets>) {
                    if (!services.export_presets) {
                        return fail(not_available("the export presets"));
                    }
                    return ok(Reply{ ExportPresetsView{ services.export_presets() } });
                } else if constexpr (std::is_same_v<T, PreviewTime>) {
                    if (!services.preview_time) {
                        return fail(not_available("the preview"));
                    }
                    return ok(Reply{ Playhead{ services.preview_time() } });
                } else if constexpr (std::is_same_v<T, ScopesSnapshot>) {
                    if (!services.scopes_snapshot) {
                        return fail(not_available("the scopes readback"));
                    }
                    return services.scopes_snapshot(alternative);
                } else if constexpr (std::is_same_v<T, EditSelection>) {
                    if (!services.edit_selection) {
                        return fail(not_available("the selection"));
                    }
                    return ok(Reply{ SelectionView{ services.edit_selection() } });
                } else if constexpr (std::is_same_v<T, MediaList>) {
                    if (!services.media_list) {
                        return fail(not_available("the media bin"));
                    }
                    return ok(Reply{ media_list_view(services.media_list()) });
                } else if constexpr (std::is_same_v<T, GpuStatus>) {
                    if (!services.gpu_status) {
                        return fail(not_available("the hardware probe"));
                    }
                    return ok(Reply{ services.gpu_status() });
                } else if constexpr (std::is_same_v<T, UiPaint>) {
                    if (!services.ui_paint) {
                        return fail(not_available("the paint overlay"));
                    }
                    return services.ui_paint(alternative);
                } else if constexpr (std::is_same_v<T, ExtensionsRequest>) {
                    if (!services.extensions) {
                        return fail(not_available("the extension store"));
                    }
                    return services.extensions(alternative);
                } else if constexpr (std::is_same_v<T, ConfigExport>) {
                    if (!services.config_export) {
                        return fail(not_available("the config store"));
                    }
                    return services.config_export(alternative);
                } else if constexpr (std::is_same_v<T, ConfigImport>) {
                    if (!services.config_import) {
                        return fail(not_available("the config store"));
                    }
                    return services.config_import(alternative);
                } else if constexpr (std::is_same_v<T, ExtensionsCatalog>) {
                    if (!services.extensions_catalog) {
                        return fail(not_available("the catalog fetcher"));
                    }
                    return services.extensions_catalog(alternative);
                } else if constexpr (std::is_same_v<T, UpdatesApply>) {
                    if (!services.updates_apply) {
                        return fail(not_available("the updater"));
                    }
                    return services.updates_apply(alternative);
                } else if constexpr (std::is_same_v<T, AiRequest>) {
                    if (!services.ai) {
                        return fail(not_available("the AI service"));
                    }
                    return services.ai(alternative);
                } else {
                    static_assert(detail::dependent_false_v<T>, "request method has no handler");
                }
            },
            request.value);
}

} // namespace genesis::api
