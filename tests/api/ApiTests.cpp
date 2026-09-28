// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "api/Api.h"
#include "api/Codec.h"
#include "api/Requests.h"
#include "extensions/Config.h"
#include "extensions/Install.h"
#include "extensions/Manager.h"
#include "project/Document.h"
#include "project/Editor.h"
#include "render/TitleTemplates.h"
#include "workspace/AssetPaths.h"

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
    check(api::method_name(Request{ api::PreviewFrame{ } }) == "preview.frame",
          "method_name: preview.frame");
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

    refuses_as_not_available(Request{ api::ProjectList{ } }, "project.list");
    refuses_as_not_available(Request{ api::TemplateInstantiate{ } }, "template.instantiate");
    refuses_as_not_available(Request{ api::TemplateSave{ "", "name" } }, "template.save");
    refuses_as_not_available(Request{ api::ExportRun{ "", api::ExportSpecRequest{ } } },
                             "export.run");
    refuses_as_not_available(Request{ api::ExportCancel{ "j1" } }, "export.cancel");
    refuses_as_not_available(Request{ api::ExportStatus{ "j1" } }, "export.status");
    refuses_as_not_available(Request{ api::PreviewFrame{ } }, "preview.frame");
    refuses_as_not_available(Request{ api::PreviewTime{ } }, "preview.time");
    refuses_as_not_available(Request{ api::ScopesSnapshot{ "histogram" } }, "scopes.snapshot");
    refuses_as_not_available(Request{ api::EditSelection{ } }, "edit.selection");
    refuses_as_not_available(Request{ api::MediaList{ } }, "media.list");
    refuses_as_not_available(Request{ api::GpuStatus{ } }, "gpu.status");
    refuses_as_not_available(Request{ api::ExtensionsCatalog{ "https://x" } },
                             "extensions.catalog");
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

    for (const char *method : { "edit.selection", "preview.time", "media.list", "gpu.status" }) {
        check(api::decode_request(method, nlohmann::json::object(), error).has_value(),
              std::string(method) + " decodes");
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

    const nlohmann::json profile{ { "format", 1 },
                                  { "appVersion", "0.1.0" },
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
        out << "format = 1\n\n"
               "[extension]\n"
               "id = \"ext.api\"\n"
               "name = \"Fixture\"\n"
               "kind = \"native\"\n"
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
        out << "format = 1\n\n"
               "[extension]\n"
               "id = \"ext.api\"\n"
               "name = \"Fixture\"\n"
               "kind = \"native\"\n"
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
      "format": 1,
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

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
