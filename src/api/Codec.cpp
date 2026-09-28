// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "api/Codec.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "api/Api.h"
#include "extensions/Sources.h"
#include "project/Document.h"
#include "project/command/Codec.h"

namespace genesis::api {

// Aliases so the moved codec bodies keep their `api::` / `project::` prefixes
// verbatim from when they lived in genesis::cli.
namespace api = genesis::api;
namespace project = genesis::project;

namespace {

using nlohmann::json;

std::string string_at(const json &obj, const char *key)
{
    if (!obj.is_object()) {
        return { };
    }
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) {
        return { };
    }
    return it->get<std::string>();
}

std::optional<std::string> opt_string_at(const json &obj, const char *key)
{
    if (!obj.is_object()) {
        return std::nullopt;
    }
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) {
        return std::nullopt;
    }
    return it->get<std::string>();
}

bool bool_at(const json &obj, const char *key, bool fallback = false)
{
    if (!obj.is_object()) {
        return fallback;
    }
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_boolean()) {
        return fallback;
    }
    return it->get<bool>();
}

std::optional<bool> opt_bool_at(const json &obj, const char *key)
{
    if (!obj.is_object()) {
        return std::nullopt;
    }
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_boolean()) {
        return std::nullopt;
    }
    return it->get<bool>();
}

std::optional<std::uint32_t> opt_u32_at(const json &obj, const char *key)
{
    if (!obj.is_object()) {
        return std::nullopt;
    }
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_number_unsigned()) {
        return std::nullopt;
    }
    return it->get<std::uint32_t>();
}

std::optional<std::uint8_t> opt_u8_at(const json &obj, const char *key)
{
    if (const std::optional<std::uint32_t> value = opt_u32_at(obj, key)) {
        return static_cast<std::uint8_t>(*value);
    }
    return std::nullopt;
}

std::optional<std::int64_t> opt_i64_at(const json &obj, const char *key)
{
    if (!obj.is_object()) {
        return std::nullopt;
    }
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_number_integer()) {
        return std::nullopt;
    }
    return it->get<std::int64_t>();
}

std::optional<double> opt_double_at(const json &obj, const char *key)
{
    if (!obj.is_object()) {
        return std::nullopt;
    }
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_number()) {
        return std::nullopt;
    }
    return it->get<double>();
}

project::VideoSettings video_from(const json &obj)
{
    project::VideoSettings video;
    if (const auto width = opt_u32_at(obj, "width")) {
        video.width = *width;
    }
    if (const auto height = opt_u32_at(obj, "height")) {
        video.height = *height;
    }
    if (const auto rate_num = opt_i64_at(obj, "rateNum")) {
        video.rate_num = *rate_num;
    }
    if (const auto rate_den = opt_i64_at(obj, "rateDen")) {
        video.rate_den = *rate_den;
    }
    return video;
}

api::ExportSpecRequest export_spec_from(const json &obj)
{
    api::ExportSpecRequest spec;
    spec.output = string_at(obj, "output");
    spec.crf = opt_u8_at(obj, "crf");
    spec.preset = opt_string_at(obj, "preset");
    spec.width = opt_u32_at(obj, "width");
    spec.height = opt_u32_at(obj, "height");
    spec.rate_num = opt_i64_at(obj, "rateNum");
    spec.rate_den = opt_i64_at(obj, "rateDen");
    spec.codec = opt_string_at(obj, "codec");
    spec.ten_bit = opt_bool_at(obj, "tenBit");
    spec.color_range = opt_string_at(obj, "colorRange");
    spec.hdr = opt_bool_at(obj, "hdr");
    return spec;
}

// The member `key` of `obj` when it is an object, else the shared empty object.
const json &object_at(const json &obj, const char *key)
{
    static const json empty = json::object();
    if (!obj.is_object()) {
        return empty;
    }
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_object()) {
        return empty;
    }
    return *it;
}

// Reads an `extensions.install` source: a string is a directory path; an object
// is a `dir` or `git` source with a non-empty `url` (and optional `ref`/
// `subdir`, which narrow a git checkout). Mirrors Catalog.cpp's read_source so
// the install verb and the catalog agree on the source shape. Returns false and
// leaves `out` untouched when the value is neither.
bool read_install_source(const json &value, extensions::InstallSource &out)
{
    if (value.is_string()) {
        out = extensions::InstallSource{ extensions::SourceType::Dir, value.get<std::string>() };
        return true;
    }
    if (!value.is_object()) {
        return false;
    }
    const auto type_it = value.find("type");
    if (type_it == value.end() || !type_it->is_string()) {
        return false;
    }
    const std::optional<extensions::SourceType> type =
            extensions::parse_source_type(type_it->get<std::string>());
    if (!type || (*type != extensions::SourceType::Dir && *type != extensions::SourceType::Git)) {
        return false;
    }
    const auto url_it = value.find("url");
    if (url_it == value.end() || !url_it->is_string() || url_it->get<std::string>().empty()) {
        return false;
    }
    out.type = *type;
    out.url = url_it->get<std::string>();
    if (const auto ref = opt_string_at(value, "ref")) {
        out.ref = *ref;
    }
    if (const auto subdir = opt_string_at(value, "subdir")) {
        out.subdir = *subdir;
    }
    return true;
}

const char *media_kind_name(project::MediaKind kind)
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

const char *color_space_name(project::ColorSpace space)
{
    switch (space) {
    case project::ColorSpace::Sdr:
        return "sdr";
    case project::ColorSpace::Hlg:
        return "hlg";
    case project::ColorSpace::Pq:
        return "pq";
    }
    return "sdr";
}

json settings_view_json(const api::SettingsView &settings)
{
    return json{ { "name", settings.name },
                 { "width", settings.width },
                 { "height", settings.height },
                 { "rateNum", settings.rate_num },
                 { "rateDen", settings.rate_den } };
}

json editor_view_json(const api::EditorView &view)
{
    json out;
    out["project"] = project::to_document(view.project);
    out["canUndo"] = view.can_undo;
    out["canRedo"] = view.can_redo;
    out["settings"] = settings_view_json(view.settings);
    out["createdId"] = view.created_id ? json(*view.created_id) : json(nullptr);
    return out;
}

json project_info_json(const api::ProjectInfo &info)
{
    return json{ { "path", info.path },         { "name", info.name },
                 { "width", info.width },       { "height", info.height },
                 { "rateNum", info.rate_num },  { "rateDen", info.rate_den },
                 { "openedAt", info.opened_at } };
}

json video_stream_json(const api::VideoStreamInfo &video)
{
    json out;
    out["index"] = video.index;
    out["codec"] = video.codec;
    out["width"] = video.width;
    out["height"] = video.height;
    out["frameRate"] = video.frame_rate;
    out["frameRateFraction"] = video.frame_rate_fraction;
    out["colorRange"] = video.color_range ? json(*video.color_range) : json(nullptr);
    out["colorSpace"] = color_space_name(video.color_space);
    return out;
}

json audio_stream_json(const api::AudioStreamInfo &audio)
{
    return json{ { "index", audio.index },
                 { "codec", audio.codec },
                 { "sampleRate", audio.sample_rate },
                 { "channels", audio.channels },
                 { "title", audio.title },
                 { "language", audio.language } };
}

json media_summary_json(const api::MediaSummary &summary)
{
    json out;
    out["path"] = summary.path;
    out["duration"] = summary.duration ? json(*summary.duration) : json(nullptr);
    out["kind"] = media_kind_name(summary.kind);
    out["video"] = summary.video ? video_stream_json(*summary.video) : json(nullptr);
    out["audio"] = summary.audio ? audio_stream_json(*summary.audio) : json(nullptr);
    json tracks = json::array();
    for (const api::AudioStreamInfo &track : summary.audio_tracks) {
        tracks.push_back(audio_stream_json(track));
    }
    out["audioTracks"] = std::move(tracks);
    return out;
}

json package_info_json(const api::PackageInfo &info)
{
    json out;
    out["id"] = info.id;
    out["name"] = info.name;
    out["kind"] = info.kind;
    out["category"] = info.category;
    out["description"] = info.description;
    out["intensity"] = info.intensity ? json(*info.intensity) : json(nullptr);
    json params = json::array();
    for (const api::ParamInfo &param : info.params) {
        params.push_back(json{ { "key", param.key },
                               { "label", param.label },
                               { "kind", param.kind },
                               { "min", param.min },
                               { "max", param.max },
                               { "default", param.default_value },
                               { "step", param.step },
                               { "unit", param.unit },
                               { "animate", param.animate },
                               { "values", param.values },
                               { "labels", param.labels } });
    }
    out["params"] = std::move(params);
    return out;
}

json slot_info_json(const api::SlotInfo &slot)
{
    return json{ { "mediaId", slot.media_id },
                 { "name", slot.name },
                 { "kind", media_kind_name(slot.kind) },
                 { "seconds", slot.seconds } };
}

json template_info_json(const api::TemplateInfo &info)
{
    json slots = json::array();
    for (const api::SlotInfo &slot : info.slot_infos) {
        slots.push_back(slot_info_json(slot));
    }
    return json{ { "path", info.path },         { "name", info.name },
                 { "width", info.width },       { "height", info.height },
                 { "rateNum", info.rate_num },  { "rateDen", info.rate_den },
                 { "slots", std::move(slots) }, { "hasPoster", info.has_poster } };
}

// Encodes a dispatch reply as the JSON its method documents.
struct ReplyToJson
{
    json operator()(const api::VersionInfo &info) const
    {
        return json{ { "apiVersion", info.api_version },
                     { "concat", info.concat },
                     { "dirs", { { "config", info.dirs.config }, { "data", info.dirs.data } } },
                     { "capabilities", info.capabilities } };
    }
    json operator()(const api::EditorView &view) const { return editor_view_json(view); }
    json operator()(const std::vector<api::ProjectInfo> &projects) const
    {
        json out = json::array();
        for (const api::ProjectInfo &info : projects) {
            out.push_back(project_info_json(info));
        }
        return out;
    }
    json operator()(const nlohmann::json &document) const { return document; }
    json operator()(const api::MediaSummary &summary) const { return media_summary_json(summary); }
    json operator()(const std::vector<api::PackageInfo> &packages) const
    {
        json out = json::array();
        for (const api::PackageInfo &info : packages) {
            out.push_back(package_info_json(info));
        }
        return out;
    }
    json operator()(const std::vector<api::TemplateInfo> &templates) const
    {
        json out = json::array();
        for (const api::TemplateInfo &info : templates) {
            out.push_back(template_info_json(info));
        }
        return out;
    }
    json operator()(const api::TemplateInfo &info) const { return template_info_json(info); }
    json operator()(const api::Started &started) const
    {
        return json{ { "job", started.job },
                     { "path", started.path },
                     { "output", started.output } };
    }
    json operator()(const api::Written &written) const
    {
        return json{ { "path", written.path },
                     { "width", written.width },
                     { "height", written.height } };
    }
    json operator()(const api::Picture &picture) const
    {
        return json{ { "width", picture.width },
                     { "height", picture.height },
                     { "png", picture.png } };
    }
    json operator()(const api::ExtensionsInstalled &installed) const
    {
        json extensions = json::array();
        for (const api::ExtensionInfo &info : installed.extensions) {
            extensions.push_back(json{ { "id", info.id },
                                       { "name", info.name },
                                       { "version", info.version },
                                       { "origin", info.origin },
                                       { "state", info.state },
                                       { "reason", info.reason },
                                       { "requires", info.dependencies },
                                       { "dependents", info.dependents } });
        }
        return json{ { "extensions", std::move(extensions) } };
    }
    json operator()(const api::ExportStatusView &view) const
    {
        return json{ { "job", view.job },
                     { "running", view.running },
                     { "progress", view.progress },
                     { "status", view.status },
                     { "error", view.error } };
    }
    json operator()(const api::SelectionView &view) const
    {
        return json{ { "clipId", view.clip_id } };
    }
    json operator()(const api::Playhead &playhead) const
    {
        return json{ { "seconds", playhead.seconds } };
    }
    json operator()(const api::MediaListView &list) const
    {
        json media = json::array();
        for (const api::MediaListEntry &entry : list.media) {
            media.push_back(
                    json{ { "id", entry.id },
                          { "path", entry.path },
                          { "name", entry.name },
                          { "kind", entry.kind },
                          { "duration", entry.duration ? json(*entry.duration) : json(nullptr) },
                          { "width", entry.width ? json(*entry.width) : json(nullptr) },
                          { "height", entry.height ? json(*entry.height) : json(nullptr) } });
        }
        return json{ { "media", std::move(media) } };
    }
    json operator()(const api::GpuStatusView &view) const
    {
        return json{ { "renderer", view.renderer },
                     { "softwareGl", view.software_gl },
                     { "decode", view.decode },
                     { "encode", view.encode },
                     { "encodeElement", view.encode_element },
                     { "ai", view.ai } };
    }
    json operator()(const api::ConfigImportView &view) const
    {
        json extensions = json::array();
        for (const api::ConfigImportEntry &entry : view.extensions) {
            extensions.push_back(json{
                    { "id", entry.id }, { "status", entry.status }, { "reason", entry.reason } });
        }
        return json{ { "settings", view.settings }, { "extensions", std::move(extensions) } };
    }
    json operator()(const api::Done &) const { return json::object(); }
};

// The structural limits a JSON-RPC request must fit under. They are checked on
// the raw bytes, before nlohmann's recursive parser runs, so a hostile request
// cannot drive the parser into unbounded recursion (a stack-exhaustion vector)
// or make it chew through an unbounded buffer.
constexpr int kMaxJsonDepth = 64;

} // namespace

// The JSON-RPC error and result envelopes, shared across every transport.
nlohmann::json wire_error(const nlohmann::json &id, std::int64_t code, const std::string &message)
{
    return nlohmann::json{ { "jsonrpc", "2.0" },
                           { "id", id },
                           { "error", { { "code", code }, { "message", message } } } };
}

nlohmann::json wire_result(nlohmann::json id, const nlohmann::json &result)
{
    return nlohmann::json{ { "jsonrpc", "2.0" }, { "id", std::move(id) }, { "result", result } };
}

// Maps one wire method name and its params onto the API's typed request. An
// unknown method, or one whose params have no JSON codec here, fills `error`
// and returns nullopt.
std::optional<Request> decode_request(const std::string &method, const json &params,
                                      DecodeError &error)
{
    if (method == "version") {
        return api::Request{ api::Version{ } };
    }
    if (method == "project.create") {
        api::ProjectCreate create;
        create.location = string_at(params, "location");
        create.name = string_at(params, "name");
        if (params.contains("video")) {
            create.video = video_from(object_at(params, "video"));
        }
        return api::Request{ std::move(create) };
    }
    if (method == "project.open") {
        return api::Request{ api::ProjectOpen{ string_at(params, "path") } };
    }
    if (method == "project.close") {
        return api::Request{ api::ProjectClose{ string_at(params, "path"),
                                                bool_at(params, "save") } };
    }
    if (method == "project.list") {
        return api::Request{ api::ProjectList{ } };
    }
    if (method == "project.get") {
        return api::Request{ api::ProjectGet{ string_at(params, "path") } };
    }
    if (method == "project.document") {
        return api::Request{ api::ProjectDocument{ string_at(params, "path") } };
    }
    if (method == "project.save") {
        api::ProjectSave save;
        save.path = string_at(params, "path");
        save.name = opt_string_at(params, "name");
        return api::Request{ std::move(save) };
    }
    if (method == "project.setVideo") {
        api::ProjectSetVideo set;
        set.path = string_at(params, "path");
        set.video = video_from(object_at(params, "video"));
        return api::Request{ std::move(set) };
    }
    if (method == "edit.apply") {
        const auto it = params.find("command");
        if (it == params.end()) {
            error = DecodeError{ api::number(api::ErrorCode::Invalid),
                                 "edit.apply: command does not decode" };
            return std::nullopt;
        }
        std::optional<project::Command> command = project::command::decode(*it);
        if (!command) {
            error = DecodeError{ api::number(api::ErrorCode::Invalid),
                                 "edit.apply: command does not decode" };
            return std::nullopt;
        }
        api::EditApply apply{ string_at(params, "path"), std::move(*command) };
        return api::Request{ std::move(apply) };
    }
    if (method == "edit.undo") {
        return api::Request{ api::EditUndo{ string_at(params, "path") } };
    }
    if (method == "edit.redo") {
        return api::Request{ api::EditRedo{ string_at(params, "path") } };
    }
    if (method == "edit.selection") {
        return api::Request{ api::EditSelection{ } };
    }
    if (method == "media.probe") {
        return api::Request{ api::MediaProbe{ string_at(params, "path") } };
    }
    if (method == "media.import") {
        return api::Request{ api::MediaImport{ string_at(params, "path"),
                                               string_at(params, "file") } };
    }
    if (method == "media.list") {
        return api::Request{ api::MediaList{ } };
    }
    if (method == "catalogue.list") {
        api::CatalogueList list;
        list.kind = opt_string_at(params, "kind");
        return api::Request{ std::move(list) };
    }
    if (method == "template.list") {
        return api::Request{ api::TemplateList{ } };
    }
    if (method == "template.fill") {
        api::TemplateFill fill;
        fill.template_ = string_at(params, "template");
        if (const auto it = params.find("texts"); it != params.end() && it->is_array()) {
            for (const json &entry : *it) {
                if (entry.is_string()) {
                    fill.texts.push_back(entry.get<std::string>());
                }
            }
        }
        return api::Request{ std::move(fill) };
    }
    if (method == "template.instantiate") {
        api::TemplateInstantiate instantiate;
        instantiate.template_ = string_at(params, "template");
        instantiate.location = string_at(params, "location");
        instantiate.name = string_at(params, "name");
        if (const auto it = params.find("fills"); it != params.end() && it->is_array()) {
            for (const json &entry : *it) {
                api::Fill fill;
                fill.media_id = string_at(entry, "mediaId");
                fill.file = string_at(entry, "file");
                instantiate.fills.push_back(std::move(fill));
            }
        }
        return api::Request{ std::move(instantiate) };
    }
    if (method == "template.save") {
        return api::Request{ api::TemplateSave{ string_at(params, "path"),
                                                string_at(params, "name") } };
    }
    if (method == "export.run") {
        api::ExportRun run;
        run.path = string_at(params, "path");
        run.spec = export_spec_from(object_at(params, "spec"));
        return api::Request{ std::move(run) };
    }
    if (method == "export.cancel") {
        return api::Request{ api::ExportCancel{ string_at(params, "job") } };
    }
    if (method == "export.status") {
        return api::Request{ api::ExportStatus{ string_at(params, "job") } };
    }
    if (method == "preview.frame") {
        api::PreviewFrame frame;
        frame.path = string_at(params, "path");
        if (const auto time = opt_double_at(params, "time")) {
            frame.time = *time;
        }
        frame.output = opt_string_at(params, "output");
        frame.width = opt_u32_at(params, "width");
        frame.height = opt_u32_at(params, "height");
        return api::Request{ std::move(frame) };
    }
    if (method == "preview.time") {
        return api::Request{ api::PreviewTime{ } };
    }
    if (method == "scopes.snapshot") {
        return api::Request{ api::ScopesSnapshot{ string_at(params, "kind") } };
    }
    if (method == "gpu.status") {
        return api::Request{ api::GpuStatus{ } };
    }
    if (method == "extensions.list") {
        return api::Request{ api::ExtensionsRequest{ api::ExtensionsList{ } } };
    }
    if (method == "extensions.remove") {
        return api::Request{ api::ExtensionsRequest{
                api::ExtensionsRemove{ string_at(params, "id") } } };
    }
    if (method == "extensions.enable") {
        return api::Request{ api::ExtensionsRequest{
                api::ExtensionsEnable{ string_at(params, "id") } } };
    }
    if (method == "extensions.disable") {
        return api::Request{ api::ExtensionsRequest{
                api::ExtensionsDisable{ string_at(params, "id") } } };
    }
    if (method == "extensions.restore") {
        return api::Request{ api::ExtensionsRequest{
                api::ExtensionsRestore{ string_at(params, "id") } } };
    }
    if (method == "extensions.install") {
        api::ExtensionsInstall install;
        if (const auto origin = opt_string_at(params, "origin")) {
            if (*origin != "builtin" && *origin != "curated" && *origin != "developer"
                && *origin != "user") {
                error = DecodeError{ api::number(api::ErrorCode::Invalid),
                                     "extensions.install: unknown origin '" + *origin + "'" };
                return std::nullopt;
            }
            install.origin = *origin;
        }
        const auto source_it = params.find("source");
        if (source_it == params.end() || !read_install_source(*source_it, install.source)) {
            error = DecodeError{ api::number(api::ErrorCode::Invalid),
                                 "extensions.install: source must be a directory path or a "
                                 "{type, url} object" };
            return std::nullopt;
        }
        return api::Request{ api::ExtensionsRequest{ std::move(install) } };
    }
    if (method == "config.export") {
        return api::Request{ api::ConfigExport{ } };
    }
    if (method == "config.import") {
        // The profile rides inline; an absent or non-object profile decodes to
        // an empty object, which import refuses as unreadable.
        const auto it = params.find("profile");
        api::ConfigImport import;
        if (it != params.end() && it->is_object()) {
            import.profile = *it;
        }
        return api::Request{ std::move(import) };
    }
    if (method == "extensions.catalog") {
        return api::Request{ api::ExtensionsCatalog{ string_at(params, "url") } };
    }
    error = DecodeError{ -32601, "method not found: " + method };
    return std::nullopt;
}

std::string oversize_request_reply()
{
    return wire_error(nullptr, -32700, "request exceeds the 1 MiB size limit").dump();
}

std::optional<std::string> json_structure_guard(const std::string &line)
{
    if (line.size() > kMaxJsonRequestBytes) {
        return oversize_request_reply();
    }
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (const char c : line) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            if (++depth > kMaxJsonDepth) {
                return wire_error(nullptr, -32700, "request nests deeper than 64 levels").dump();
            }
        } else if (c == '}' || c == ']') {
            --depth;
        }
    }
    return std::nullopt;
}

nlohmann::json reply_to_json(const Reply &reply)
{
    return std::visit(ReplyToJson{ }, reply);
}

} // namespace genesis::api
