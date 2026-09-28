// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "api/Api.h"
#include "api/Codec.h"
#include "api/PathPolicy.h"
#include "api/Requests.h"
#include "extensions/Config.h"
#include "extensions/Install.h"
#include "extensions/Manager.h"
#include "project/Document.h"
#include "project/Editor.h"
#include "render/ExportPresets.h"
#include "render/TitleTemplates.h"
#include "workspace/AssetPaths.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

namespace api = genesis::api;
namespace extensions = genesis::extensions;
namespace project = genesis::project;

using api::dispatch;
using api::EditorView;
using api::MediaSummary;
using api::PackageInfo;
using api::Request;
using api::Response;
using api::Services;
using api::VersionInfo;

// Services a test wires in: a probe that reports a fixed video file and a
// catalogue that hands back one effect pack.
Services default_services()
{
    Services services;
    services.probe = [](const std::string &path) -> std::optional<project::NewMedia> {
        project::NewMedia media;
        media.path = path;
        media.name = "probe.mp4";
        media.duration = genesis::core::Rational::from_int(10);
        media.kind = project::MediaKind::Video;
        media.width = 1920;
        media.height = 1080;
        media.frame_rate = 30.0;
        media.frame_rate_fraction = "30/1";
        media.video_codec = "h264";
        return media;
    };
    services.catalogue = [](const std::optional<std::string> &) {
        std::vector<genesis::effects::Pack> packs;
        genesis::effects::Pack pack;
        pack.id = "author.effect";
        pack.name = "Effect";
        pack.kind = genesis::effects::Kind::Effect;
        packs.push_back(std::move(pack));
        return packs;
    };
    return services;
}

void test_version()
{
    project::Editor editor;
    Response response = dispatch(Request{ api::Version{ } }, editor, default_services());
    check(response.has_value(), "version returns a result");
    if (const VersionInfo *info = std::get_if<VersionInfo>(&*response)) {
        check(info->api_version == std::string(api::API_VERSION),
              "version carries the API version");
    } else {
        check(false, "version reply is a VersionInfo");
    }
}

void test_method_names()
{
    check(api::method_name(Request{ api::Version{ } }) == "version", "method_name: version");
    check(api::method_name(Request{ api::ProjectCreate{ } }) == "project.create",
          "method_name: project.create");
    check(api::method_name(
                  Request{ api::EditApply{ "", project::Command{ project::AddTrack{ } } } })
                  == "edit.apply",
          "method_name: edit.apply");
}

void test_edit_apply_and_undo()
{
    project::Editor editor;
    const Services services = default_services();
    const project::Project before = editor.project();

    Response applied =
            dispatch(Request{ api::EditApply{ "", project::Command{ project::AddTrack{ } } } },
                     editor, services);
    check(applied.has_value(), "edit.apply succeeds");
    check(editor.project().active().tracks.size() == before.active().tracks.size() + 1,
          "edit.apply added a track");

    Response undone = dispatch(Request{ api::EditUndo{ "" } }, editor, services);
    check(undone.has_value(), "edit.undo succeeds");
    check(editor.project() == before, "edit.undo restores the project exactly");
}

void test_get_and_document_leave_project_untouched()
{
    project::Editor editor;
    const Services services = default_services();
    const project::Project before = editor.project();

    Response got = dispatch(Request{ api::ProjectGet{ "" } }, editor, services);
    Response doc = dispatch(Request{ api::ProjectDocument{ "" } }, editor, services);
    check(got.has_value(), "project.get succeeds");
    check(doc.has_value(), "project.document succeeds");
    check(editor.project() == before, "get and document leave the project byte-identical");

    if (const nlohmann::json *json = std::get_if<nlohmann::json>(&*doc)) {
        check(*json == project::to_document(before),
              "project.document is exactly a save's document");
    } else {
        check(false, "project.document reply is a Document");
    }
}

void test_save_open_round_trip()
{
    project::Editor editor;
    const Services services = default_services();
    (void)dispatch(Request{ api::EditApply{ "", project::Command{ project::AddTrack{ } } } },
                   editor, services);

    // temp_directory_path(), not a hard-coded scratch dir: the dev host's
    // /tmp/opencode does not exist on a runner, and a test that only passes
    // on the machine it was written on is not a test.
    const std::string path =
            (std::filesystem::temp_directory_path() / "genesis-api-roundtrip.json").string();
    Response saved = dispatch(Request{ api::ProjectSave{ path, std::nullopt } }, editor, services);
    check(saved.has_value(), "project.save succeeds");
    check(!editor.dirty(), "project.save marks the project saved");

    const project::Project expected = editor.project();
    Response opened = dispatch(Request{ api::ProjectOpen{ path } }, editor, services);
    check(opened.has_value(), "project.open succeeds");
    if (const EditorView *view = std::get_if<EditorView>(&*opened)) {
        check(view->project == expected, "save->open round-trips exactly");
    } else {
        check(false, "project.open reply is an EditorView");
    }
}

void test_media_and_catalogue()
{
    project::Editor editor;
    const Services services = default_services();

    Response probed = dispatch(Request{ api::MediaProbe{ "/x/probe.mp4" } }, editor, services);
    check(probed.has_value() && std::holds_alternative<MediaSummary>(*probed),
          "media.probe returns a MediaSummary");

    Response imported =
            dispatch(Request{ api::MediaImport{ "", "/x/probe.mp4" } }, editor, services);
    check(imported.has_value(), "media.import succeeds");
    check(editor.project().media.size() == 1, "media.import added one bin item");

    Response packages = dispatch(Request{ api::CatalogueList{ std::nullopt } }, editor, services);
    check(packages.has_value() && std::holds_alternative<std::vector<PackageInfo>>(*packages),
          "catalogue.list returns packages");
}

void test_not_available()
{
    project::Editor editor;
    const Services services = default_services();

    const auto refuses_as_not_available = [&](Request request, const std::string &what) {
        Response response = dispatch(request, editor, services);
        if (response) {
            check(false, what + " returns an error");
            return;
        }
        check(response.error().code == api::ErrorCode::NotAvailable,
              what + " is typed NotAvailable");
    };

    refuses_as_not_available(Request{ api::ExportRun{ "", api::ExportSpecRequest{ } } },
                             "export.run");
    refuses_as_not_available(Request{ api::ExportCancel{ "j1" } }, "export.cancel");
    refuses_as_not_available(Request{ api::ExportStatus{ "j1" } }, "export.status");
    refuses_as_not_available(Request{ api::ExportPresets{ } }, "export.presets");
    refuses_as_not_available(Request{ api::PreviewTime{ } }, "preview.time");
    refuses_as_not_available(Request{ api::ScopesSnapshot{ "histogram" } }, "scopes.snapshot");
    refuses_as_not_available(Request{ api::EditSelection{ } }, "edit.selection");
    refuses_as_not_available(Request{ api::MediaList{ } }, "media.list");
    refuses_as_not_available(Request{ api::GpuStatus{ } }, "gpu.status");
    refuses_as_not_available(Request{ api::ExtensionsCatalog{ "https://x" } },
                             "extensions.catalog");
    refuses_as_not_available(Request{ api::UiPaint{ true, std::nullopt } }, "ui.paint");
}

void test_new_method_names()
{
    check(api::method_name(Request{ api::ExportStatus{ "j1" } }) == "export.status",
          "method_name: export.status");
    check(api::method_name(Request{ api::PreviewTime{ } }) == "preview.time",
          "method_name: preview.time");
    check(api::method_name(Request{ api::ScopesSnapshot{ "histogram" } }) == "scopes.snapshot",
          "method_name: scopes.snapshot");
    check(api::method_name(Request{ api::EditSelection{ } }) == "edit.selection",
          "method_name: edit.selection");
    check(api::method_name(Request{ api::MediaList{ } }) == "media.list",
          "method_name: media.list");
    check(api::method_name(Request{ api::GpuStatus{ } }) == "gpu.status",
          "method_name: gpu.status");
    check(api::method_name(Request{ api::UpdatesApply{ true } }) == "updates.apply",
          "method_name: updates.apply");
    check(api::method_name(Request{ api::UiPaint{ true, std::nullopt } }) == "ui.paint",
          "method_name: ui.paint");
}

void test_new_codec_round_trips()
{
    api::DecodeError error;

    const std::optional<api::Request> status =
            api::decode_request("export.status", { { "job", "j1" } }, error);
    check(status.has_value(), "export.status decodes");
    if (status) {
        const auto *s = std::get_if<api::ExportStatus>(&status->value);
        check(s != nullptr && s->job == "j1", "export.status decodes to ExportStatus with the job");
    }

    const std::optional<api::Request> snapshot =
            api::decode_request("scopes.snapshot", { { "kind", "waveform" } }, error);
    check(snapshot.has_value(), "scopes.snapshot decodes");
    if (snapshot) {
        const auto *s = std::get_if<api::ScopesSnapshot>(&snapshot->value);
        check(s != nullptr && s->kind == "waveform", "scopes.snapshot carries the kind");
    }

    const std::optional<api::Request> parade =
            api::decode_request("scopes.snapshot", { { "kind", "parade" } }, error);
    check(parade.has_value(), "scopes.snapshot (parade) decodes");
    if (parade) {
        const auto *s = std::get_if<api::ScopesSnapshot>(&parade->value);
        check(s != nullptr && s->kind == "parade", "scopes.snapshot carries the parade kind");
    }

    for (const char *method : { "edit.selection", "preview.time", "media.list", "gpu.status" }) {
        check(api::decode_request(method, nlohmann::json::object(), error).has_value(),
              std::string(method) + " decodes");
    }

    const std::optional<api::Request> apply =
            api::decode_request("updates.apply", { { "confirm", true } }, error);
    check(apply.has_value(), "updates.apply decodes");
    if (apply) {
        const auto *u = std::get_if<api::UpdatesApply>(&apply->value);
        check(u != nullptr && u->confirm, "updates.apply carries the confirm flag");
    }
}

void test_new_host_methods_with_seams()
{
    project::Editor editor;
    Services services = default_services();

    // Import one media item so media.list has something to report.
    (void)dispatch(Request{ api::MediaImport{ "", "/x/probe.mp4" } }, editor, services);

    services.export_status = [](const api::ExportStatus &request) {
        api::ExportStatusView view;
        view.job = request.job;
        view.running = true;
        view.progress = 0.5;
        view.status = "Exporting";
        return api::Response{ api::Reply{ std::move(view) } };
    };
    services.edit_selection = [] { return std::string("c1"); };
    services.preview_time = [] { return 3.5; };
    services.media_list = [&editor] { return editor.project().media; };
    services.gpu_status = [] {
        api::GpuStatusView view;
        view.renderer = "llvmpipe";
        view.software_gl = true;
        view.decode = "software";
        view.encode = "software";
        view.ai = "CPU";
        return view;
    };

    Response status = dispatch(Request{ api::ExportStatus{ "j1" } }, editor, services);
    check(status.has_value(), "export.status with a seam succeeds");
    if (const auto *view = std::get_if<api::ExportStatusView>(&*status)) {
        check(view->job == "j1" && view->running && view->progress == 0.5,
              "export.status carries the run state");
    } else {
        check(false, "export.status reply is an ExportStatusView");
    }

    Response selection = dispatch(Request{ api::EditSelection{ } }, editor, services);
    check(selection.has_value(), "edit.selection with a seam succeeds");
    if (const auto *view = std::get_if<api::SelectionView>(&*selection)) {
        check(view->clip_id == "c1", "edit.selection carries the clip id");
    } else {
        check(false, "edit.selection reply is a SelectionView");
    }

    Response time = dispatch(Request{ api::PreviewTime{ } }, editor, services);
    check(time.has_value(), "preview.time with a seam succeeds");
    if (const auto *view = std::get_if<api::Playhead>(&*time)) {
        check(view->seconds == 3.5, "preview.time carries the position");
    } else {
        check(false, "preview.time reply is a Playhead");
    }

    Response list = dispatch(Request{ api::MediaList{ } }, editor, services);
    check(list.has_value(), "media.list with a seam succeeds");
    if (const auto *view = std::get_if<api::MediaListView>(&*list)) {
        check(view->media.size() == 1, "media.list lists the one bin item");
        if (view->media.size() == 1) {
            check(view->media[0].kind == "video", "media.list spells the kind");
        }
    } else {
        check(false, "media.list reply is a MediaListView");
    }

    Response gpu = dispatch(Request{ api::GpuStatus{ } }, editor, services);
    check(gpu.has_value(), "gpu.status with a seam succeeds");
    if (const auto *view = std::get_if<api::GpuStatusView>(&*gpu)) {
        check(view->renderer == "llvmpipe" && view->software_gl, "gpu.status carries the probe");
    } else {
        check(false, "gpu.status reply is a GpuStatusView");
    }

    // updates.apply: the seam is the user's consent, so a confirmed call
    // succeeds and an unconfirmed one refuses, proving both flow through
    // dispatch to the app-side seam.
    services.updates_apply = [](const api::UpdatesApply &request) {
        if (!request.confirm) {
            return api::Response{ std::unexpected(
                    api::ApiError{ api::ErrorCode::Refused, "the update was not confirmed" }) };
        }
        return api::Response{ api::Done{ } };
    };
    Response apply = dispatch(Request{ api::UpdatesApply{ true } }, editor, services);
    check(apply.has_value(), "a confirmed updates.apply with a seam succeeds");
    Response refuse = dispatch(Request{ api::UpdatesApply{ false } }, editor, services);
    check(!refuse.has_value(), "an unconfirmed updates.apply refuses");
}

void test_scopes_snapshot_seam_round_trips()
{
    project::Editor editor;
    Services services = default_services();

    // The app wires the real scopes readback; here the seam records the kind it
    // was asked for and echoes it back, proving a parade request reaches the
    // seam with its kind intact (the reply shape itself is built app-side). The
    // HDR fields (hdr + scaleMax) are part of that app-side shape, so the seam
    // also echoes them to prove they pass through the dispatch round-trip.
    std::string seen_kind;
    services.scopes_snapshot = [&seen_kind](const api::ScopesSnapshot &request) {
        seen_kind = request.kind;
        return api::Response{ api::Reply{ nlohmann::json{
                { "kind", request.kind }, { "hdr", true }, { "scaleMax", 10000 } } } };
    };

    Response response = dispatch(Request{ api::ScopesSnapshot{ "parade" } }, editor, services);
    check(response.has_value(), "scopes.snapshot (parade) with a seam succeeds");
    check(seen_kind == "parade", "the parade kind reaches the scopes seam");
    if (response) {
        if (const auto *doc = std::get_if<nlohmann::json>(&*response)) {
            check(doc->is_object() && (*doc)["kind"] == "parade",
                  "the seam's reply passes through for parade");
            check((*doc)["hdr"] == true, "the HDR flag passes through for the panel");
            check((*doc)["scaleMax"] == 10000, "the nit scale max passes through for the panel");
        } else {
            check(false, "scopes.snapshot reply is a json object");
        }
    }
}

void test_ui_paint_codec_and_dispatch()
{
    api::DecodeError error;

    // The codec reads `active` (required) and an optional `clipId`.
    const std::optional<api::Request> paint =
            api::decode_request("ui.paint", { { "active", true }, { "clipId", "c1" } }, error);
    check(paint.has_value(), "ui.paint decodes");
    if (paint) {
        const auto *p = std::get_if<api::UiPaint>(&paint->value);
        check(p != nullptr && p->active && p->clip_id == std::optional<std::string>{ "c1" },
              "ui.paint carries active + clipId");
        check(api::method_name(*paint) == "ui.paint", "ui.paint method name round-trips");
    }

    const std::optional<api::Request> paint_min =
            api::decode_request("ui.paint", { { "active", true } }, error);
    check(paint_min.has_value(), "ui.paint (no clipId) decodes");
    if (paint_min) {
        const auto *p = std::get_if<api::UiPaint>(&paint_min->value);
        check(p != nullptr && p->active && !p->clip_id, "ui.paint without clipId has none");
    }

    // With a seam, the ask flows through dispatch field for field.
    project::Editor editor;
    Services services = default_services();
    bool seen_active = false;
    std::optional<std::string> seen_clip_id;
    services.ui_paint = [&](const api::UiPaint &request) {
        seen_active = request.active;
        seen_clip_id = request.clip_id;
        return api::Response{ api::Done{ } };
    };

    Response response = dispatch(Request{ api::UiPaint{ true, "c1" } }, editor, services);
    check(response.has_value(), "ui.paint with a seam succeeds");
    check(seen_active && seen_clip_id == std::optional<std::string>{ "c1" },
          "ui.paint routes active + clipId through");
}

void test_extensions_method_names()
{
    check(api::method_name(Request{ api::ExtensionsRequest{ api::ExtensionsList{ } } })
                  == "extensions.list",
          "method_name: extensions.list");
    check(api::method_name(
                  Request{ api::ExtensionsRequest{ api::ExtensionsRemove{ "ext.fixture" } } })
                  == "extensions.remove",
          "method_name: extensions.remove");
    check(api::method_name(
                  Request{ api::ExtensionsRequest{ api::ExtensionsEnable{ "ext.fixture" } } })
                  == "extensions.enable",
          "method_name: extensions.enable");
    check(api::method_name(
                  Request{ api::ExtensionsRequest{ api::ExtensionsDisable{ "ext.fixture" } } })
                  == "extensions.disable",
          "method_name: extensions.disable");
    check(api::method_name(
                  Request{ api::ExtensionsRequest{ api::ExtensionsRestore{ "ext.fixture" } } })
                  == "extensions.restore",
          "method_name: extensions.restore");
    check(api::method_name(Request{ api::ExtensionsRequest{ api::ExtensionsInstall{
                  extensions::InstallSource{ extensions::SourceType::Dir, "/x" },
                  std::nullopt } } })
                  == "extensions.install",
          "method_name: extensions.install");
}

void test_extensions_not_available_without_seam()
{
    project::Editor editor;
    const Services services = default_services();

    Response response =
            dispatch(Request{ api::ExtensionsRequest{ api::ExtensionsList{ } } }, editor, services);
    check(!response.has_value(), "extensions.list without a seam errors");
    if (!response) {
        check(response.error().code == api::ErrorCode::NotAvailable, "and is typed NotAvailable");
    }
}

void test_extensions_seam_round_trips()
{
    project::Editor editor;
    const std::filesystem::path store =
            std::filesystem::temp_directory_path() / "genesis-api-extensions";
    std::filesystem::remove_all(store);
    std::filesystem::create_directories(store);

    Services services = default_services();
    services.extensions = api::extensions_seam(std::make_shared<extensions::Manager>(store));

    Response listed =
            dispatch(Request{ api::ExtensionsRequest{ api::ExtensionsList{ } } }, editor, services);
    check(listed.has_value() && std::holds_alternative<api::ExtensionsInstalled>(*listed),
          "extensions.list over an empty store returns ExtensionsInstalled");
    if (const auto *installed = std::get_if<api::ExtensionsInstalled>(&*listed)) {
        check(installed->extensions.empty(), "with no extensions installed");
    }

    std::filesystem::remove_all(store);
}

void test_remaining_methods_dispatch()
{
    project::Editor editor;
    const Services services = default_services();

    check(dispatch(Request{ api::ProjectCreate{ "loc", "name", std::nullopt } }, editor, services)
                  .has_value(),
          "project.create returns a view");
    check(dispatch(Request{ api::ProjectSetVideo{ "", project::VideoSettings{ } } }, editor,
                   services)
                  .has_value(),
          "project.setVideo succeeds");
    check(dispatch(Request{ api::ProjectClose{ "", false } }, editor, services).has_value(),
          "project.close returns done");
    check(dispatch(Request{ api::EditRedo{ "" } }, editor, services).has_value(),
          "edit.redo succeeds");
}

void test_open_refuses_a_too_deep_document()
{
    project::Editor editor;
    const Services services = default_services();

    // A document nesting past the 512-level cap is refused on its raw bytes,
    // before the recursive parser can be driven into unbounded recursion.
    std::string deep;
    for (int i = 0; i < 600; ++i) {
        deep += '[';
    }
    for (int i = 0; i < 600; ++i) {
        deep += ']';
    }
    const std::string path =
            (std::filesystem::temp_directory_path() / "genesis-api-deep.json").string();
    {
        std::ofstream out(path);
        out << deep;
    }

    Response opened = dispatch(Request{ api::ProjectOpen{ path } }, editor, services);
    check(!opened.has_value(), "a too-deep document is refused, not parsed");
    std::filesystem::remove(path);
}

void test_template_codec_round_trips()
{
    api::DecodeError error;

    const std::optional<api::Request> list =
            api::decode_request("template.list", nlohmann::json::object(), error);
    check(list.has_value(), "template.list decodes");
    if (list) {
        check(std::holds_alternative<api::TemplateList>(list->value),
              "template.list decodes to TemplateList");
        check(api::method_name(*list) == "template.list", "template.list method name round-trips");
    }

    const nlohmann::json params = { { "template", "title-basic" },
                                    { "texts", nlohmann::json::array({ "First", "Second" }) } };
    const std::optional<api::Request> fill = api::decode_request("template.fill", params, error);
    check(fill.has_value(), "template.fill decodes");
    if (fill) {
        const auto *f = std::get_if<api::TemplateFill>(&fill->value);
        check(f != nullptr, "template.fill decodes to TemplateFill");
        if (f) {
            check(f->template_ == "title-basic", "the template name round-trips");
            check(f->texts == std::vector<std::string>{ "First", "Second" },
                  "the texts round-trip in order");
        }
        check(api::method_name(*fill) == "template.fill", "template.fill method name round-trips");
    }
}

void test_template_list_and_fill()
{
    project::Editor editor;
    const Services services = default_services();

    // Ground truth from the same asset root dispatch reads, so the assertions
    // hold against whatever templates are shipped. The assets are pinned by
    // the title_templates suite; here the library's presence is enough.
    const genesis::render::TemplateLibrary library =
            genesis::render::discover_templates(genesis::workspace::asset_root() / "titles");
    if (library.templates.empty()) {
        std::printf("skipping template list/fill: no templates discovered\n");
        return;
    }

    Response listed = dispatch(Request{ api::TemplateList{ } }, editor, services);
    check(listed.has_value(), "template.list succeeds");
    const nlohmann::json *list_json = listed ? std::get_if<nlohmann::json>(&*listed) : nullptr;
    check(list_json != nullptr, "template.list returns a JSON document");
    if (!list_json) {
        return;
    }
    check(list_json->is_array(), "template.list returns an array");
    check(list_json->size() == library.templates.size(),
          "template.list lists every discovered template");
    if (list_json->size() != library.templates.size()) {
        return;
    }

    const genesis::render::TitleTemplate &tmpl = library.templates.front();
    std::vector<std::string> texts;
    texts.reserve(tmpl.text_slots.size());
    for (const genesis::render::TitleSlot &slot : tmpl.text_slots) {
        texts.push_back("T" + std::to_string(slot.index));
    }

    Response filled = dispatch(Request{ api::TemplateFill{ tmpl.name, texts } }, editor, services);
    check(filled.has_value(), "template.fill succeeds for a known template");
    const nlohmann::json *fill_json = filled ? std::get_if<nlohmann::json>(&*filled) : nullptr;
    check(fill_json != nullptr, "template.fill returns a JSON document");
    if (!fill_json) {
        return;
    }
    check(fill_json->is_array(), "template.fill returns an array");
    check(fill_json->size() == tmpl.text_slots.size(), "template.fill returns one pair per slot");
    for (std::size_t i = 0; i < fill_json->size(); ++i) {
        const nlohmann::json &pair = (*fill_json)[i];
        check(pair.contains("text") && pair.contains("style"), "each slot carries text and style");
        if (!pair.contains("text") || !pair.contains("style")) {
            continue;
        }
        check(pair["text"] == texts[i], "the slot text lands in order");
        check(pair["style"]["content"] == texts[i],
              "the style content matches the text (addTextClip-ready)");
        check(pair["style"].contains("fontFamily"), "the style carries its styling metadata");
    }

    Response missing =
            dispatch(Request{ api::TemplateFill{ "no-such-template", { } } }, editor, services);
    check(!missing.has_value(), "template.fill for an unknown template errors");
    if (!missing) {
        check(missing.error().code == api::ErrorCode::NotFound, "and is typed NotFound");
    }
}

void test_config_method_names()
{
    check(api::method_name(Request{ api::ConfigExport{ } }) == "config.export",
          "method_name: config.export");
    check(api::method_name(Request{ api::ConfigImport{ nlohmann::json::object() } })
                  == "config.import",
          "method_name: config.import");
}

void test_config_codec_round_trips()
{
    api::DecodeError error;

    const std::optional<api::Request> exported =
            api::decode_request("config.export", nlohmann::json::object(), error);
    check(exported.has_value(), "config.export decodes");
    if (exported) {
        check(std::holds_alternative<api::ConfigExport>(exported->value),
              "config.export decodes to ConfigExport");
    }

    const nlohmann::json profile{ { "appVersion", "0.1.0" },
                                  { "settings", nlohmann::json::object() },
                                  { "extensions", nlohmann::json::array() } };
    const std::optional<api::Request> imported =
            api::decode_request("config.import", { { "profile", profile } }, error);
    check(imported.has_value(), "config.import decodes");
    if (imported) {
        const auto *c = std::get_if<api::ConfigImport>(&imported->value);
        check(c != nullptr, "config.import decodes to ConfigImport");
        if (c) {
            check(c->profile == profile, "the inline profile round-trips");
        }
    }

    // A reply encodes the per-extension outcome.
    api::ConfigImportView view;
    view.settings = { "applied proxiesEnabled" };
    view.extensions = { api::ConfigImportEntry{ "ext.fixture", "installed", "" },
                        api::ConfigImportEntry{ "ext.git", "skipped",
                                                "git install not yet supported" } };
    const nlohmann::json encoded = api::reply_to_json(api::Reply{ view });
    check(encoded["extensions"].is_array() && encoded["extensions"].size() == 2,
          "the reply encodes the extension outcomes");
    check(encoded["extensions"][0]["id"] == "ext.fixture"
                  && encoded["extensions"][0]["status"] == "installed",
          "an installed entry encodes id and status");
}

void test_config_not_available_without_seam()
{
    project::Editor editor;
    const Services services = default_services();

    Response exported = dispatch(Request{ api::ConfigExport{ } }, editor, services);
    check(!exported.has_value(), "config.export without a seam errors");
    if (!exported) {
        check(exported.error().code == api::ErrorCode::NotAvailable, "and is typed NotAvailable");
    }
    Response imported =
            dispatch(Request{ api::ConfigImport{ nlohmann::json::object() } }, editor, services);
    check(!imported.has_value(), "config.import without a seam errors");
}

void test_config_seam_round_trips()
{
    project::Editor editor;

    const std::filesystem::path base =
            std::filesystem::temp_directory_path() / "genesis-api-config";
    const std::filesystem::path store_a = base / "store-a";
    const std::filesystem::path store_b = base / "store-b";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);

    // A Developer native install into store A, so the exported profile carries
    // an origin plus a reproducible Dir source.
    const std::filesystem::path source = base / "ext";
    std::filesystem::create_directories(source);
    {
        std::ofstream out(source / "extension.toml");
        out << "[extension]\n"
               "id = \"ext.api\"\n"
               "name = \"Fixture\"\n"
               "version = 1\n"
               "api = 1\n"
               "entry = \"libfixture.so\"\n";
    }
    {
        std::ofstream out(source / "libfixture.so");
        out << "placeholder";
    }
    check(extensions::install_extension(source, store_a, extensions::Origin::Developer).ok(),
          "a Developer native install seeds store A");

    Services services = default_services();
    const extensions::ConfigSeam seam; // no settings to read or apply
    services.config_export = api::config_export_seam(store_a, seam, "0.1.0");
    services.config_import = api::config_import_seam(store_b, seam);

    Response exported = dispatch(Request{ api::ConfigExport{ } }, editor, services);
    check(exported.has_value(), "config.export with a seam succeeds");
    const nlohmann::json *profile_json =
            exported ? std::get_if<nlohmann::json>(&*exported) : nullptr;
    check(profile_json != nullptr, "config.export returns a JSON profile");
    if (!profile_json) {
        std::filesystem::remove_all(base);
        return;
    }
    check((*profile_json)["extensions"].size() == 1,
          "the exported profile lists the one extension");

    Response imported = dispatch(Request{ api::ConfigImport{ *profile_json } }, editor, services);
    check(imported.has_value(), "config.import with a seam succeeds");
    if (const auto *view = std::get_if<api::ConfigImportView>(&*imported)) {
        check(view->extensions.size() == 1, "the import reports one extension");
        if (view->extensions.size() == 1) {
            check(view->extensions[0].status == "installed",
                  "and the Developer extension installs into store B");
        }
    } else {
        check(false, "config.import reply is a ConfigImportView");
    }
    check(extensions::installed_packs(store_b).count("ext.api") == 1,
          "store B now holds the extension");

    std::filesystem::remove_all(base);
}

void test_extensions_install_codec_round_trips()
{
    api::DecodeError error;

    // The inner ExtensionsInstall sits one level down: outer Request holds the
    // ExtensionsRequest wrapper, which holds the verb.
    const auto install_from = [](const api::Request &request) -> const api::ExtensionsInstall * {
        const auto *ext = std::get_if<api::ExtensionsRequest>(&request.value);
        return ext ? std::get_if<api::ExtensionsInstall>(&ext->value) : nullptr;
    };

    // A string source is a directory path under the Dir type, with no origin.
    const std::optional<api::Request> dir =
            api::decode_request("extensions.install", { { "source", "/some/dir" } }, error);
    check(dir.has_value(), "extensions.install with a string source decodes");
    if (dir) {
        const api::ExtensionsInstall *i = install_from(*dir);
        check(i != nullptr, "decodes to ExtensionsInstall");
        if (i) {
            check(i->source.type == extensions::SourceType::Dir && i->source.url == "/some/dir"
                          && i->source.ref.empty() && i->source.subdir.empty(),
                  "the string source is a Dir source with the path as url");
            check(!i->origin.has_value(), "no origin reads as absent");
        }
        check(api::method_name(*dir) == "extensions.install",
              "extensions.install method name round-trips");
    }

    // An object source carries type/url/ref/subdir, plus a named origin.
    const nlohmann::json git = { { "type", "git" },
                                 { "url", "https://example.com/ext.git" },
                                 { "ref", "v1" },
                                 { "subdir", "packs" } };
    const std::optional<api::Request> git_req = api::decode_request(
            "extensions.install", { { "source", git }, { "origin", "curated" } }, error);
    check(git_req.has_value(), "extensions.install with a git object decodes");
    if (git_req) {
        const api::ExtensionsInstall *i = install_from(*git_req);
        check(i != nullptr, "decodes to ExtensionsInstall");
        if (i) {
            check(i->source.type == extensions::SourceType::Git
                          && i->source.url == "https://example.com/ext.git" && i->source.ref == "v1"
                          && i->source.subdir == "packs",
                  "the git source round-trips its fields");
            check(i->origin == std::optional<std::string>{ "curated" }, "the origin round-trips");
        }
    }

    // An object dir source (no ref/subdir) reads as Dir.
    const std::optional<api::Request> obj_dir = api::decode_request(
            "extensions.install", { { "source", { { "type", "dir" }, { "url", "/x" } } } }, error);
    check(obj_dir.has_value(), "an object dir source decodes");
    if (obj_dir) {
        const api::ExtensionsInstall *i = install_from(*obj_dir);
        check(i != nullptr && i->source.type == extensions::SourceType::Dir
                      && i->source.url == "/x",
              "the object dir source reads as Dir");
    }

    // Refusals decode to an Invalid error, not a typed request.
    const auto refuse = [&](const nlohmann::json &params, const std::string &what) {
        const std::optional<api::Request> request =
                api::decode_request("extensions.install", params, error);
        check(!request.has_value(), what + " is refused");
        if (!request) {
            check(error.code == api::number(api::ErrorCode::Invalid), "and typed Invalid");
        }
    };
    refuse({ { "source", "/x" }, { "origin", "nonsense" } }, "an unknown origin");
    refuse({ { "source", { { "type", "builtin" }, { "url", "x" } } } }, "a builtin source type");
    refuse({ { "source", { { "type", "git" } } } }, "a git source with no url");
    refuse(nlohmann::json::object(), "a missing source");
    refuse({ { "source", 42 } }, "a non-string non-object source");
}

void test_extensions_install_dispatch()
{
    project::Editor editor;
    const std::filesystem::path base =
            std::filesystem::temp_directory_path() / "genesis-api-install";
    const std::filesystem::path store = base / "store";
    const std::filesystem::path source = base / "ext";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);

    // A Developer native install source: a manifest plus a placeholder library,
    // the shape install_extension accepts (consent-gated, no dlopen here).
    std::filesystem::create_directories(source);
    {
        std::ofstream out(source / "extension.toml");
        out << "[extension]\n"
               "id = \"ext.api\"\n"
               "name = \"Fixture\"\n"
               "version = 1\n"
               "api = 1\n"
               "entry = \"libfixture.so\"\n";
    }
    {
        std::ofstream out(source / "libfixture.so");
        out << "placeholder";
    }

    Services services = default_services();
    services.extensions = api::extensions_seam(std::make_shared<extensions::Manager>(store));

    api::ExtensionsInstall install;
    install.source = extensions::InstallSource{ .type = extensions::SourceType::Dir,
                                                .url = source.string() };
    install.origin = "developer";

    Response installed =
            dispatch(Request{ api::ExtensionsRequest{ std::move(install) } }, editor, services);
    check(installed.has_value(), "extensions.install with a seam succeeds");
    if (const nlohmann::json *json =
                installed ? std::get_if<nlohmann::json>(&*installed) : nullptr) {
        check(json != nullptr, "the reply is a JSON document");
        check(json->contains("installed"), "the reply carries the installed path");
    } else {
        check(false, "extensions.install reply is a JSON document");
    }
    check(extensions::installed_packs(store).count("ext.api") == 1,
          "the store now holds the extension");

    // A missing directory is a typed Refused carrying the problem.
    api::ExtensionsInstall bad;
    bad.source = extensions::InstallSource{ .type = extensions::SourceType::Dir,
                                            .url = (base / "missing").string() };
    Response refused =
            dispatch(Request{ api::ExtensionsRequest{ std::move(bad) } }, editor, services);
    check(!refused.has_value(), "a missing directory install is refused");
    if (!refused) {
        check(refused.error().code == api::ErrorCode::Refused, "and typed Refused");
        check(!refused.error().message.empty(), "with the refusal reason");
    }

    std::filesystem::remove_all(base);
}

void test_extensions_catalog_dispatch()
{
    project::Editor editor;
    Services services = default_services();

    // Without a seam the catalog is NotAvailable.
    Response missing = dispatch(Request{ api::ExtensionsCatalog{ "https://x" } }, editor, services);
    check(!missing.has_value(), "extensions.catalog without a seam errors");
    if (!missing) {
        check(missing.error().code == api::ErrorCode::NotAvailable, "and is typed NotAvailable");
    }
    // A seam over a fake fetcher parses and lists the entries.
    const std::string body = R"({
      "entries": [
        {"id": "author.effect", "name": "Effect", "version": "1.0.0",
         "description": "An effect",
         "source": {"type": "git", "url": "https://example.com/ext.git"}}
      ]
    })";
    services.extensions_catalog = api::extensions_catalog_seam(
            [&body](const std::string &url) -> std::optional<std::string> {
                if (url == "https://catalog.example/genesis.json") {
                    return body;
                }
                return std::nullopt;
            });

    Response listed =
            dispatch(Request{ api::ExtensionsCatalog{ "https://catalog.example/genesis.json" } },
                     editor, services);
    check(listed.has_value(), "extensions.catalog with a seam succeeds");
    if (const nlohmann::json *json = std::get_if<nlohmann::json>(&*listed)) {
        check(json->is_array() && json->size() == 1, "the catalog lists one entry");
        if (json->is_array() && json->size() == 1) {
            check((*json)[0]["id"] == "author.effect", "the entry id round-trips");
        }
    } else {
        check(false, "extensions.catalog reply is a JSON document");
    }

    // A fetch that fails (or a bad url) yields a Failed error, not a crash.
    Response failed = dispatch(Request{ api::ExtensionsCatalog{ "https://x" } }, editor, services);
    check(!failed.has_value(), "a failed catalog fetch errors");
    if (!failed) {
        check(failed.error().code == api::ErrorCode::Failed, "and is typed Failed");
    }
}

void test_export_presets_codec_round_trips()
{
    api::DecodeError error;

    const std::optional<api::Request> presets =
            api::decode_request("export.presets", nlohmann::json::object(), error);
    check(presets.has_value(), "export.presets decodes");
    if (presets) {
        check(std::holds_alternative<api::ExportPresets>(presets->value),
              "export.presets decodes to ExportPresets");
        check(api::method_name(*presets) == "export.presets",
              "export.presets method name round-trips");
    }

    api::ExportPresetsView view;
    view.presets.push_back(
            api::ExportPresetInfo{ "vp8", 1920, 1080, 30, 1, 30.0, "vp8", "3 Mb/s" });
    const nlohmann::json encoded = api::reply_to_json(api::Reply{ std::move(view) });
    check(encoded.contains("presets") && encoded["presets"].is_array(),
          "the reply carries a presets array");
    if (encoded.contains("presets") && encoded["presets"].is_array()
        && encoded["presets"].size() == 1) {
        const nlohmann::json &preset = encoded["presets"][0];
        check(preset["name"] == "vp8", "a preset encodes its name");
        check(preset["width"] == 1920 && preset["height"] == 1080, "a preset encodes its frame");
        check(preset["rateNum"] == 30 && preset["rateDen"] == 1, "a preset encodes its exact rate");
        check(preset["fps"] == 30.0, "a preset encodes its fps");
        check(preset["codec"] == "vp8", "a preset encodes its codec family");
        check(preset["quality"] == "3 Mb/s", "a preset encodes its quality hint");
    }
}

void test_export_presets_dispatch_with_seam()
{
    project::Editor editor;
    Services services = default_services();

    services.export_presets = [] {
        std::vector<api::ExportPresetInfo> presets;
        presets.push_back(api::ExportPresetInfo{ "vp8", 1920, 1080, 30, 1, 30.0, "vp8", "3 Mb/s" });
        presets.push_back(
                api::ExportPresetInfo{ "h264", 1920, 1080, 30, 1, 30.0, "h264", "23 crf" });
        return presets;
    };

    Response listed = dispatch(Request{ api::ExportPresets{ } }, editor, services);
    check(listed.has_value(), "export.presets with a seam succeeds");
    if (const auto *view = std::get_if<api::ExportPresetsView>(&*listed)) {
        check(view->presets.size() == 2, "the seam lists two presets");
        if (view->presets.size() == 2) {
            check(view->presets[0].name == "vp8" && view->presets[0].codec == "vp8",
                  "the first preset carries its name and codec");
            check(view->presets[1].name == "h264" && view->presets[1].quality == "23 crf",
                  "the second preset carries its name and quality");
        }
    } else {
        check(false, "export.presets reply is an ExportPresetsView");
    }
}

void test_export_presets_app_bound_list()
{
    const std::vector<api::ExportPresetInfo> presets = api::export_presets_view();
    check(presets.size() == genesis::render::export_presets().size(),
          "the app-bound list mirrors the render catalogue exactly");
    check(!presets.empty(), "the app-bound list is non-empty");

    const auto find = [&](const std::string &name) -> const api::ExportPresetInfo * {
        for (const api::ExportPresetInfo &preset : presets) {
            if (preset.name == name) {
                return &preset;
            }
        }
        return nullptr;
    };

    const api::ExportPresetInfo *vp8 = find("vp8");
    check(vp8 != nullptr, "vp8 is listed");
    if (vp8) {
        check(vp8->width == 1920 && vp8->height == 1080, "vp8 carries its frame");
        check(vp8->rate_num == 30 && vp8->rate_den == 1 && vp8->fps == 30.0,
              "vp8 carries 30 fps exactly");
        check(vp8->codec == "vp8", "vp8 carries its codec family");
        check(vp8->quality == "3 Mb/s", "vp8 carries its quality hint");
    }

    const api::ExportPresetInfo *h264 = find("h264");
    check(h264 != nullptr && h264->codec == "h264", "h264 carries its codec family");
}

void test_ai_method_names()
{
    check(api::method_name(Request{ api::AiRequest{ api::AiModels{ } } }) == "ai.models",
          "method_name: ai.models");
    check(api::method_name(Request{ api::AiRequest{ api::AiDownload{ "tts" } } }) == "ai.download",
          "method_name: ai.download");
    check(api::method_name(Request{ api::AiRequest{ api::AiStatus{ } } }) == "ai.status",
          "method_name: ai.status");
    check(api::method_name(Request{ api::AiRequest{ api::AiRun{ } } }) == "ai.run",
          "method_name: ai.run");
    check(api::method_name(Request{ api::AiRequest{ api::AiCancel{ "tts" } } }) == "ai.cancel",
          "method_name: ai.cancel");
}

void test_ai_status_state()
{
    check(api::ai_status_state("Idle", "") == "idle", "Idle reads idle");
    check(api::ai_status_state("Consent", "") == "idle", "Consent (awaiting a human) reads idle");
    check(api::ai_status_state("Running", "downloading model") == "downloading",
          "Running + downloading model reads downloading");
    check(api::ai_status_state("Running", "transcribing") == "running",
          "Running + any other stage reads running");
    check(api::ai_status_state("Done", "") == "done", "Done reads done");
    check(api::ai_status_state("Failed", "") == "failed", "Failed reads failed");
    check(api::ai_status_state("Cancelled", "") == "failed", "Cancelled reads failed");
}

void test_ai_codec_round_trips()
{
    api::DecodeError error;

    const auto inner = [](const std::optional<api::Request> &request) -> const api::AiRequest * {
        return request ? std::get_if<api::AiRequest>(&request->value) : nullptr;
    };

    const std::optional<api::Request> models =
            api::decode_request("ai.models", nlohmann::json::object(), error);
    check(models.has_value(), "ai.models decodes");
    if (const api::AiRequest *req = inner(models)) {
        check(std::holds_alternative<api::AiModels>(req->value), "ai.models decodes to AiModels");
    }
    check(models.has_value() && api::method_name(*models) == "ai.models",
          "ai.models method name round-trips");

    const std::optional<api::Request> download =
            api::decode_request("ai.download", { { "kind", "tts" } }, error);
    check(download.has_value(), "ai.download decodes");
    if (const api::AiRequest *req = inner(download)) {
        const auto *d = std::get_if<api::AiDownload>(&req->value);
        check(d != nullptr && d->kind == "tts", "ai.download decodes to AiDownload with the kind");
    }

    const std::optional<api::Request> status =
            api::decode_request("ai.status", nlohmann::json::object(), error);
    check(status.has_value(), "ai.status decodes");
    if (const api::AiRequest *req = inner(status)) {
        check(std::holds_alternative<api::AiStatus>(req->value), "ai.status decodes to AiStatus");
    }

    const nlohmann::json run_params = { { "kind", "cutout" },
                                        { "clipId", "c1" },
                                        { "params", { { "subject", "person" } } } };
    const std::optional<api::Request> run = api::decode_request("ai.run", run_params, error);
    check(run.has_value(), "ai.run decodes");
    if (const api::AiRequest *req = inner(run)) {
        const auto *r = std::get_if<api::AiRun>(&req->value);
        check(r != nullptr, "ai.run decodes to AiRun");
        if (r) {
            check(r->kind == "cutout" && r->clip_id == std::optional<std::string>{ "c1" }
                          && r->params["subject"] == "person",
                  "ai.run carries kind/clipId/params");
        }
    }
    check(run.has_value() && api::method_name(*run) == "ai.run", "ai.run method name round-trips");

    const nlohmann::json enhance_run_params = { { "kind", "enhance" },
                                                { "clipId", "c1" },
                                                { "params", { { "factor", 4 } } } };
    const std::optional<api::Request> enhance_run =
            api::decode_request("ai.run", enhance_run_params, error);
    check(enhance_run.has_value(), "ai.run (enhance) decodes");
    if (const api::AiRequest *req = inner(enhance_run)) {
        const auto *r = std::get_if<api::AiRun>(&req->value);
        check(r != nullptr && r->kind == "enhance"
                      && r->clip_id == std::optional<std::string>{ "c1" }
                      && r->params["factor"] == 4,
              "ai.run enhance carries clipId + the factor");
    }

    const std::optional<api::Request> cancel =
            api::decode_request("ai.cancel", { { "kind", "captions" } }, error);
    check(cancel.has_value(), "ai.cancel decodes");
    if (const api::AiRequest *req = inner(cancel)) {
        const auto *c = std::get_if<api::AiCancel>(&req->value);
        check(c != nullptr && c->kind == "captions", "ai.cancel decodes to AiCancel with the kind");
    }

    // The reply views encode to their documented shapes.
    api::AiModelsView models_view;
    models_view.models.push_back(
            api::AiModelInfo{ "captions", "whisper", true, 42, "MIT", "https://x" });
    models_view.voices.push_back(api::AiVoiceInfo{ "Speaker 1", 0, false });
    models_view.voices.push_back(api::AiVoiceInfo{ "alice", -1, true });
    const nlohmann::json models_json = api::reply_to_json(api::Reply{ models_view });
    check(models_json["models"].is_array() && models_json["models"].size() == 1,
          "ai.models reply encodes a models array");
    if (models_json["models"].size() == 1) {
        const nlohmann::json &m = models_json["models"][0];
        check(m["kind"] == "captions" && m["model"] == "whisper" && m["installed"] == true
                      && m["sizeBytes"] == 42 && m["license"] == "MIT" && m["url"] == "https://x",
              "an ai model encodes kind/model/installed/size/license/url");
    }
    check(models_json["voices"].is_array() && models_json["voices"].size() == 2,
          "ai.models reply encodes a voices array");
    if (models_json["voices"].size() == 2) {
        const nlohmann::json &speaker = models_json["voices"][0];
        const nlohmann::json &cloned = models_json["voices"][1];
        check(speaker["name"] == "Speaker 1" && speaker["index"] == 0 && speaker["cloned"] == false,
              "a Kokoro voice encodes name/index/cloned");
        check(cloned["name"] == "alice" && cloned["index"] == -1 && cloned["cloned"] == true,
              "a cloned voice encodes name/index/cloned");
    }

    api::AiStatusView status_view;
    status_view.entries.push_back(api::AiStatusEntry{ "tts", "downloading", 0.5, "" });
    const nlohmann::json status_json = api::reply_to_json(api::Reply{ status_view });
    check(status_json["entries"].is_array() && status_json["entries"].size() == 1,
          "ai.status reply encodes an entries array");
    if (status_json["entries"].size() == 1) {
        const nlohmann::json &e = status_json["entries"][0];
        check(e["kind"] == "tts" && e["state"] == "downloading" && e["progress"] == 0.5
                      && e["error"] == "",
              "an ai status entry encodes kind/state/progress/error");
    }
}

void test_ai_not_available_without_seam()
{
    project::Editor editor;
    const Services services = default_services();

    const auto refuses = [&](api::Request request, const std::string &what) {
        Response response = dispatch(request, editor, services);
        check(!response.has_value(), what + " without a seam errors");
        if (!response) {
            check(response.error().code == api::ErrorCode::NotAvailable,
                  what + " is typed NotAvailable");
        }
    };

    refuses(Request{ api::AiRequest{ api::AiModels{ } } }, "ai.models");
    refuses(Request{ api::AiRequest{ api::AiDownload{ "tts" } } }, "ai.download");
    refuses(Request{ api::AiRequest{ api::AiStatus{ } } }, "ai.status");
    refuses(Request{ api::AiRequest{ api::AiRun{ } } }, "ai.run");
    refuses(Request{ api::AiRequest{ api::AiCancel{ "tts" } } }, "ai.cancel");
}

void test_ai_dispatch_with_seam()
{
    project::Editor editor;
    Services services = default_services();

    api::AiModelsView models;
    models.models.push_back(
            api::AiModelInfo{ "captions", "whisper", false, 147964211, "MIT", "https://x" });
    api::AiStatusView status;
    status.entries.push_back(api::AiStatusEntry{ "captions", "idle", 0.0, "" });

    // The request the seam received, so the routing can be asserted field for
    // field rather than only by the canned reply.
    std::string seen_verb;
    std::string seen_kind;
    std::optional<std::string> seen_clip_id;
    nlohmann::json seen_params;

    services.ai = [&](const api::AiRequest &request) {
        return std::visit(
                [&](const auto &alternative) -> api::Response {
                    using T = std::decay_t<decltype(alternative)>;
                    if constexpr (std::is_same_v<T, api::AiModels>) {
                        seen_verb = "models";
                        return api::Response{ api::Reply{ models } };
                    } else if constexpr (std::is_same_v<T, api::AiStatus>) {
                        seen_verb = "status";
                        return api::Response{ api::Reply{ status } };
                    } else if constexpr (std::is_same_v<T, api::AiDownload>) {
                        seen_verb = "download";
                        seen_kind = alternative.kind;
                        return api::Response{ api::Reply{ api::Done{ } } };
                    } else if constexpr (std::is_same_v<T, api::AiRun>) {
                        seen_verb = "run";
                        seen_kind = alternative.kind;
                        seen_clip_id = alternative.clip_id;
                        seen_params = alternative.params;
                        // Mirror the real seam's refusal: a captions run
                        // without a clip id is a typed Invalid.
                        if (alternative.kind == "captions" && !alternative.clip_id) {
                            return api::Response{ std::unexpected(api::ApiError{
                                    api::ErrorCode::Invalid, "ai.run captions requires clipId" }) };
                        }
                        return api::Response{ api::Reply{ api::Done{ } } };
                    } else {
                        seen_verb = "cancel";
                        seen_kind = alternative.kind;
                        return api::Response{ api::Reply{ api::Done{ } } };
                    }
                },
                request.value);
    };

    Response listed = dispatch(Request{ api::AiRequest{ api::AiModels{ } } }, editor, services);
    check(listed.has_value() && seen_verb == "models", "ai.models routes to the seam");
    if (const auto *view = listed ? std::get_if<api::AiModelsView>(&*listed) : nullptr) {
        check(view->models.size() == 1 && view->models[0].model == "whisper",
              "ai.models carries the models");
    } else {
        check(false, "ai.models reply is an AiModelsView");
    }

    Response status_response =
            dispatch(Request{ api::AiRequest{ api::AiStatus{ } } }, editor, services);
    check(status_response.has_value() && seen_verb == "status", "ai.status routes to the seam");
    if (const auto *view =
                status_response ? std::get_if<api::AiStatusView>(&*status_response) : nullptr) {
        check(view->entries.size() == 1 && view->entries[0].kind == "captions",
              "ai.status carries the entries");
    } else {
        check(false, "ai.status reply is an AiStatusView");
    }

    Response download =
            dispatch(Request{ api::AiRequest{ api::AiDownload{ "tts" } } }, editor, services);
    check(download.has_value() && seen_verb == "download" && seen_kind == "tts",
          "ai.download routes with the kind");

    api::AiRun run;
    run.kind = "captions";
    run.params = nlohmann::json::object();
    Response refused = dispatch(Request{ api::AiRequest{ std::move(run) } }, editor, services);
    check(!refused.has_value(), "ai.run with a missing clip id is refused");
    if (!refused) {
        check(refused.error().code == api::ErrorCode::Invalid, "and typed Invalid");
    }

    api::AiRun run2;
    run2.kind = "cutout";
    run2.clip_id = "c1";
    run2.params = { { "subject", "person" } };
    Response accepted = dispatch(Request{ api::AiRequest{ std::move(run2) } }, editor, services);
    check(accepted.has_value(), "ai.run with kind/clipId/params succeeds");
    check(seen_verb == "run" && seen_kind == "cutout"
                  && seen_clip_id == std::optional<std::string>{ "c1" }
                  && seen_params["subject"] == "person",
          "ai.run routes kind/clipId/params through");

    api::AiRun enhance;
    enhance.kind = "enhance";
    enhance.clip_id = "c1";
    enhance.params = { { "factor", 4 } };
    Response enhanced = dispatch(Request{ api::AiRequest{ std::move(enhance) } }, editor, services);
    check(enhanced.has_value(), "ai.run enhance with a factor succeeds");
    check(seen_verb == "run" && seen_kind == "enhance"
                  && seen_clip_id == std::optional<std::string>{ "c1" }
                  && seen_params["factor"] == 4,
          "ai.run routes the enhance factor through");

    Response cancel =
            dispatch(Request{ api::AiRequest{ api::AiCancel{ "enhance" } } }, editor, services);
    check(cancel.has_value() && seen_verb == "cancel" && seen_kind == "enhance",
          "ai.cancel routes with the kind");
}

void test_path_policy()
{
    const std::filesystem::path base =
            std::filesystem::temp_directory_path() / "genesis-api-path-policy";
    const std::filesystem::path allowed = base / "allowed";
    const std::filesystem::path outside = base / "outside";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(allowed);
    std::filesystem::create_directories(outside);

    // An inactive policy (no roots) confines nothing.
    api::PathPolicy open;
    check(!open.check("/anywhere.json", api::PathUse::Write).has_value(),
          "an inactive policy allows any write");
    check(!open.check("/anywhere.json", api::PathUse::Read).has_value(),
          "an inactive policy allows any read");

    // A write-only policy leaves reads permissive and confines writes.
    api::PathPolicy policy;
    policy.allow(allowed, api::PathUse::Write);
    check(!policy.check("/anywhere.json", api::PathUse::Read).has_value(),
          "a write-only policy leaves reads permissive");
    check(!policy.check((allowed / "out.json").string(), api::PathUse::Write).has_value(),
          "a write under an allowed root is accepted");

    const auto refused_outside = policy.check((outside / "x.json").string(), api::PathUse::Write);
    check(refused_outside.has_value(), "a write outside the roots is refused");
    if (refused_outside) {
        check(refused_outside->code == api::ErrorCode::Refused, "and typed Refused");
        check(refused_outside->message.find("x.json") != std::string::npos,
              "and names the refused path");
    }

    // A sibling that shares a string prefix is not "within".
    const std::filesystem::path sibling = base / "allowed-sibling";
    std::filesystem::create_directories(sibling);
    check(policy.check((sibling / "x.json").string(), api::PathUse::Write).has_value(),
          "a path under a sibling prefix is refused");

    // A `..` component is refused even where it stays on paper under the root.
    check(policy.check((allowed / ".." / "out.json").string(), api::PathUse::Write).has_value(),
          "a '..' component is refused");
    check(policy.check((allowed / ".." / "outside" / "x.json").string(), api::PathUse::Write)
                  .has_value(),
          "a '..' escape past the root is refused");

    // A symlink that points outside the roots resolves outside and is refused.
    const std::filesystem::path link = allowed / "link";
    std::error_code ec;
    std::filesystem::create_directory_symlink(outside, link, ec);
    if (!ec) {
        check(policy.check((link / "x.json").string(), api::PathUse::Write).has_value(),
              "a write through a symlink outside the roots is refused");
    }

    // `/` as a root allows anywhere.
    api::PathPolicy anywhere;
    anywhere.allow("/", api::PathUse::Write);
    check(!anywhere.check((outside / "x.json").string(), api::PathUse::Write).has_value(),
          "a '/' root allows writing anywhere");

    // check_request_paths maps the path-taking methods to their use: writes
    // are confined, reads pass through a write-only policy untouched.
    const auto save_outside = api::check_request_paths(
            Request{ api::ProjectSave{ (outside / "x.json").string(), std::nullopt } }, policy);
    check(save_outside.has_value() && save_outside->code == api::ErrorCode::Refused,
          "project.save outside the roots is refused");
    check(!api::check_request_paths(
                   Request{ api::ProjectSave{ (allowed / "x.json").string(), std::nullopt } },
                   policy)
                   .has_value(),
          "project.save under an allowed root is accepted");
    check(!api::check_request_paths(Request{ api::ProjectOpen{ "/anywhere.json" } }, policy)
                   .has_value(),
          "project.open (a read) is not confined by write roots");
    check(!api::check_request_paths(Request{ api::MediaProbe{ "/anywhere.mp4" } }, policy)
                   .has_value(),
          "media.probe (a read) is not confined by write roots");

    api::ExportSpecRequest spec;
    spec.output = (outside / "out.mp4").string();
    check(api::check_request_paths(Request{ api::ExportRun{ "", std::move(spec) } }, policy)
                  .has_value(),
          "export.run output outside the roots is refused");

    // A read-confined policy confines project.open but leaves writes open, the
    // mirror image proving the two root sets are independent.
    api::PathPolicy reads;
    reads.allow(allowed, api::PathUse::Read);
    check(reads.check((outside / "media.mp4").string(), api::PathUse::Read).has_value(),
          "a read root confines media reads");
    check(!reads.check((outside / "out.json").string(), api::PathUse::Write).has_value(),
          "a read-only policy leaves writes permissive");

    // The default server posture: writes confined to home + cwd, reads open.
    const api::PathPolicy server = api::server_path_policy();
    check(server.check((outside / "out.json").string(), api::PathUse::Write).has_value(),
          "the server policy confines writes");
    check(!server.check("/anywhere.json", api::PathUse::Read).has_value(),
          "the server policy leaves reads open");

    std::filesystem::remove_all(base);
}

} // namespace

int main()
{
    test_version();
    test_method_names();
    test_edit_apply_and_undo();
    test_get_and_document_leave_project_untouched();
    test_save_open_round_trip();
    test_media_and_catalogue();
    test_not_available();
    test_new_method_names();
    test_new_codec_round_trips();
    test_new_host_methods_with_seams();
    test_scopes_snapshot_seam_round_trips();
    test_ui_paint_codec_and_dispatch();
    test_extensions_method_names();
    test_extensions_not_available_without_seam();
    test_extensions_seam_round_trips();
    test_remaining_methods_dispatch();
    test_open_refuses_a_too_deep_document();
    test_template_codec_round_trips();
    test_template_list_and_fill();
    test_config_method_names();
    test_config_codec_round_trips();
    test_config_not_available_without_seam();
    test_config_seam_round_trips();
    test_extensions_install_codec_round_trips();
    test_extensions_install_dispatch();
    test_extensions_catalog_dispatch();
    test_export_presets_codec_round_trips();
    test_export_presets_dispatch_with_seam();
    test_export_presets_app_bound_list();
    test_ai_method_names();
    test_ai_status_state();
    test_ai_codec_round_trips();
    test_ai_not_available_without_seam();
    test_ai_dispatch_with_seam();
    test_path_policy();

    return genesis::test::summary();
}
