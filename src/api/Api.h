// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/Requests.h"
#include "effects/Pack.h"
#include "project/Project.h"
#include "project/command/MediaCommands.h"
#include "project/model/Media.h"

namespace genesis::project {
class Editor;
} // namespace genesis::project

namespace genesis::extensions {
struct ExtensionRecord;
class Manager;
struct ConfigSeam;
} // namespace genesis::extensions

namespace genesis::api {

// The version of this contract (message.rs `API_VERSION`).
inline constexpr std::string_view API_VERSION = "0.2";

// Why a request has no reply. `number()` maps each to its JSON-RPC 2.0 code.
// `NotAvailable` is a host addition: it marks a method whose feature
// (templates, export, preview, recents) is not built in this engine-free host,
// so a caller gets a typed refusal rather than a silent success.
enum class ErrorCode {
    Parse,
    Invalid,
    NotOpen,
    NotFound,
    Refused,
    Busy,
    Cancelled,
    Unauthorized,
    Failed,
    NotAvailable,
};

// The code's JSON-RPC 2.0 number: standard numbers for the standard meanings,
// server-defined ones (-32000 downwards) for the rest.
constexpr std::int64_t number(ErrorCode code)
{
    switch (code) {
    case ErrorCode::Parse:
        return -32700;
    case ErrorCode::Invalid:
        return -32600;
    case ErrorCode::Failed:
        return -32000;
    case ErrorCode::NotOpen:
        return -32001;
    case ErrorCode::NotFound:
        return -32002;
    case ErrorCode::Refused:
        return -32003;
    case ErrorCode::Busy:
        return -32004;
    case ErrorCode::Cancelled:
        return -32005;
    case ErrorCode::Unauthorized:
        return -32006;
    case ErrorCode::NotAvailable:
        return -32007;
    }
    return -32000;
}

// A refusal: what kind, and the sentence a person would be shown.
struct ApiError
{
    ErrorCode code = ErrorCode::Failed;
    std::string message;
};

// The app's directories, as paths (message.rs `Dirs`).
struct Dirs
{
    std::string config;
    std::string data;
};

// The build and the contract it speaks (message.rs `VersionInfo`).
struct VersionInfo
{
    std::string api_version;
    Dirs dirs;
    // What this build serves, as names a caller tests for membership.
    std::vector<std::string> capabilities;
};

// The session's settings, as the window shows them (message.rs `SettingsView`).
struct SettingsView
{
    std::string name;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int64_t rate_num = 0;
    std::int64_t rate_den = 0;
};

// What every mutating call returns: the authoritative state plus history
// availability (message.rs `EditorView`).
struct EditorView
{
    genesis::project::Project project;
    bool can_undo = false;
    bool can_redo = false;
    SettingsView settings;
    // The id a creating command minted, if any.
    std::optional<std::string> created_id;
};

// A video stream, as the UI sees it (message.rs `VideoStreamInfo`).
struct VideoStreamInfo
{
    std::uint32_t index = 0;
    std::string codec;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double frame_rate = 0.0;
    std::string frame_rate_fraction;
    // "limited" or "full", absent when the file says nothing.
    std::optional<std::string> color_range;
    genesis::project::ColorSpace color_space = genesis::project::ColorSpace::Sdr;
};

// An audio stream, as the UI sees it (message.rs `AudioStreamInfo`).
struct AudioStreamInfo
{
    std::uint32_t index = 0;
    std::string codec;
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;
    std::string title;
    std::string language;
};

// What a probe hands back (message.rs `MediaSummary`).
struct MediaSummary
{
    std::string path;
    // Container duration in seconds, when the container states one.
    std::optional<double> duration;
    genesis::project::MediaKind kind = genesis::project::MediaKind::Video;
    std::optional<VideoStreamInfo> video;
    std::optional<AudioStreamInfo> audio;
    std::vector<AudioStreamInfo> audio_tracks;
};

// One parameter of a package (message.rs `ParamInfo`).
struct ParamInfo
{
    std::string key;
    std::string label;
    // "float", "int", "bool", "enum", "color", "point", "wheel" or "curve".
    std::string kind;
    double min = 0.0;
    double max = 0.0;
    // The value an untouched control means (message.rs spells it `default`).
    double default_value = 0.0;
    double step = 0.0;
    std::string unit;
    bool animate = false;
    std::vector<double> values;
    std::vector<std::string> labels;
};

// One effect package, as a caller building a chain needs it (message.rs
// `PackageInfo`).
struct PackageInfo
{
    std::string id;
    std::string name;
    // "effect", "filter", "audio", "transition" or "generator".
    std::string kind;
    std::string category;
    std::string description;
    std::optional<std::string> intensity;
    std::vector<ParamInfo> params;
};

// A job the API has begun (message.rs `Started`).
struct Started
{
    std::string job;
    std::string path;
    std::string output;
};

// The empty reply, an object so every reply is one.
struct Done
{
};

// One installed native extension, as the extensions surface lists it.
struct ExtensionInfo
{
    std::string id;
    std::string name;
    std::uint32_t version = 0;
    // "builtin", "curated", "developer" or "user".
    std::string origin;
    // "enabled", "disabled" or "failed".
    std::string state;
    // Why a non-enabled extension is not enabled; empty when enabled.
    std::string reason;
    // The ids this extension depends on, in manifest order.
    std::vector<std::string> dependencies;
    // The installed ids that depend on this one.
    std::vector<std::string> dependents;
};

// The installed native-extension store, as `extensions.list` reports it.
struct ExtensionsInstalled
{
    std::vector<ExtensionInfo> extensions;
};

// One extension's import outcome, as `config.import` reports it.
struct ConfigImportEntry
{
    std::string id;
    // "installed", "skipped" or "failed".
    std::string status;
    // Why a skipped/failed extension was not installed; empty for an install.
    std::string reason;
};

// The `config.import` reply: one line per setting (applied/skipped) and one
// entry per profile extension.
struct ConfigImportView
{
    std::vector<std::string> settings;
    std::vector<ConfigImportEntry> extensions;
};

// One export run's state, as `export.status` reports it.
struct ExportStatusView
{
    std::string job;
    bool running = false;
    double progress = 0.0;
    // "Idle", "Exporting", "Done", "Cancelled" or "Failed".
    std::string status;
    std::string error;
};

// One export preset, as `export.presets` reports it. `name` is the value
// `export.run`'s `spec.preset` accepts; `codec` is the codec family in the same
// spelling `export.run`'s `spec.codec` accepts. `fps` is the convenience
// decimal of the exact `rate_num`/`rate_den` fraction the dialog shows.
struct ExportPresetInfo
{
    std::string name;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int64_t rate_num = 0;
    std::int64_t rate_den = 0;
    double fps = 0.0;
    std::string codec;
    // The medium-quality hint the dialog shows ("23 crf", "3 Mb/s", ...).
    std::string quality;
};

// The available export presets, as `export.presets` reports them.
struct ExportPresetsView
{
    std::vector<ExportPresetInfo> presets;
};

// The currently selected clip, as `edit.selection` reports it; empty id means
// no selection.
struct SelectionView
{
    std::string clip_id;
};

// The preview playhead position, as `preview.time` reports it.
struct Playhead
{
    double seconds = 0.0;
};

// One media-bin entry, as `media.list` reports it.
struct MediaListEntry
{
    std::string id;
    std::string path;
    std::string name;
    // "video", "audio" or "image".
    std::string kind;
    // Seconds; absent when the container said nothing.
    std::optional<double> duration;
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
};

// The media bin, as `media.list` reports it.
struct MediaListView
{
    std::vector<MediaListEntry> media;
};

// The hardware probe's read-only status, as `gpu.status` reports it.
struct GpuStatusView
{
    std::string renderer;
    bool software_gl = false;
    std::string decode;
    std::string encode;
    std::string encode_element;
    std::string ai;
};

// One model an AI kind runs, as `ai.models` reports it. `model` is the store's
// catalogue id (e.g. "whisper", "rvm"); a kind with several models (cutout) lists
// one entry each.
struct AiModelInfo
{
    // "captions", "cutout", "enhance" or "tts".
    std::string kind;
    // The catalogue id, the value `model_path`/`installed` key on.
    std::string model;
    bool installed = false;
    std::uint64_t size_bytes = 0;
    std::string license;
    std::string url;
};

// One selectable text-to-speech voice, as `ai.models` reports it alongside the
// tts model. A Kokoro speaker carries its integer id (`index`, the value
// `ai.run` tts takes as `params.voice`); a cloned voice carries `cloned = true`
// and `index = -1`, and is selected by name.
struct AiVoiceInfo
{
    // The display name: "Speaker 1".."Speaker N" for a Kokoro speaker, or the
    // cloned voice's captured name.
    std::string name;
    // The Kokoro speaker id, or -1 for a cloned voice.
    int index = -1;
    // True for a captured cloned voice, false for a Kokoro speaker.
    bool cloned = false;
};

// The models each AI kind runs, as `ai.models` reports them. `voices` lists the
// tts kind's selectable voices (Kokoro speakers plus cloned voices); it is
// empty for every other kind and until the tts runtime reports its speakers.
struct AiModelsView
{
    std::vector<AiModelInfo> models;
    std::vector<AiVoiceInfo> voices;
};

// One kind's run state, as `ai.status` reports it.
struct AiStatusEntry
{
    // "captions", "cutout", "enhance" or "tts".
    std::string kind;
    // "idle", "downloading", "running", "done" or "failed".
    std::string state;
    // How much of the current work has finished, 0..1.
    double progress = 0.0;
    // The last problem; empty on success.
    std::string error;
};

// Every kind's run state, as `ai.status` reports them.
struct AiStatusView
{
    std::vector<AiStatusEntry> entries;
};

// What a request hands back, as the payload alone: the variant is implied by
// the method.
using Reply = std::variant<VersionInfo, EditorView, nlohmann::json, MediaSummary,
                           std::vector<PackageInfo>, Started, ExtensionsInstalled, ExportStatusView,
                           ExportPresetsView, SelectionView, Playhead, MediaListView, GpuStatusView,
                           ConfigImportView, AiModelsView, AiStatusView, Done>;

// A request's outcome: the reply, or why there is none.
using Response = std::expected<Reply, ApiError>;

// Something that happened to a job (message.rs `Event`). Declared for wire
// completeness; the engine-free host runs no jobs, so nothing emits these yet
// - the export runner does, when it is built.
struct CutoutProgress
{
    std::string job;
    std::string path;
    std::string media_id;
    bool fetching = false;
    float fraction = 0.0F;
};

struct ExportProgress
{
    std::string job;
    std::string path;
    std::int64_t frame = 0;
    std::int64_t total = 0;
    std::string stage;
};

struct ExportDone
{
    std::string job;
    std::string path;
    std::string output;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct ExportFailed
{
    std::string job;
    std::string path;
    ApiError error;
};

using Event = std::variant<CutoutProgress, ExportProgress, ExportDone, ExportFailed>;

// The services a caller wires into the dispatcher. Injected rather than linked
// so the host library stays free of the engine adapter and the stores: a probe
// result arrives through `probe` and a package list through `catalogue`, in the
// style of `render::EffectFallback`'s lookup.
struct Services
{
    // Probes a media file into the host's model, for `media.probe` and
    // `media.import`. Absent means media requests are refused as `Failed`.
    std::function<std::optional<genesis::project::NewMedia>(const std::string &path)> probe;
    // Lists effect packages, optionally restricted to one kind, for
    // `catalogue.list`. Absent means the catalogue is refused as `Failed`.
    std::function<std::vector<genesis::effects::Pack>(const std::optional<std::string> &kind)>
            catalogue;
    // Handles the extension-management verbs (`extensions.list/remove/enable/
    // disable/restore`). Absent means they are refused as `NotAvailable`.
    std::function<Response(const ExtensionsRequest &)> extensions;
    // Runs an export (`export.run`). Absent means it is refused as
    // `NotAvailable`; present only in the app, which owns the export runner.
    std::function<Response(const ExportRun &)> export_run;
    // Cancels the running export (`export.cancel`).
    std::function<Response(const ExportCancel &)> export_cancel;
    // Reports the running (or last) export's state (`export.status`).
    std::function<Response(const ExportStatus &)> export_status;
    // Lists the available export presets (`export.presets`). Absent means it
    // is refused as `NotAvailable`; present only in the app.
    std::function<std::vector<ExportPresetInfo>()> export_presets;
    // Samples one scope of the current frame (`scopes.snapshot`).
    std::function<Response(const ScopesSnapshot &)> scopes_snapshot;
    // The currently selected clip id, empty for none (`edit.selection`).
    std::function<std::string()> edit_selection;
    // The preview playhead position in seconds (`preview.time`).
    std::function<double()> preview_time;
    // The open project's media bin (`media.list`).
    std::function<std::vector<genesis::project::MediaItem>()> media_list;
    // The hardware probe's status (`gpu.status`).
    std::function<GpuStatusView()> gpu_status;
    // Sets the stroke-painting overlay's state (`ui.paint`). Absent means it is
    // refused as `NotAvailable`; present only in the app, which owns the paint
    // bridge and the monitor overlay.
    std::function<Response(const UiPaint &)> ui_paint;
    // Exports the machine's portable config (`config.export`). Absent means it
    // is refused as `NotAvailable`.
    std::function<Response(const ConfigExport &)> config_export;
    // Imports a config profile (`config.import`). Absent means it is refused
    // as `NotAvailable`.
    std::function<Response(const ConfigImport &)> config_import;
    // Fetches and lists a marketplace catalog (`extensions.catalog`). Absent
    // means it is refused as `NotAvailable`.
    std::function<Response(const ExtensionsCatalog &)> extensions_catalog;
    // Applies the update the notify surface found (`updates.apply`). Absent
    // means it is refused as `NotAvailable`; present only in the app, which
    // owns the update controller.
    std::function<Response(const UpdatesApply &)> updates_apply;
    // Handles the AI verbs (`ai.models/download/status/run/cancel`). Absent
    // means they are refused as `NotAvailable`; present only in the app, which
    // owns the four AI controllers. The CLI stays unbound in v1.
    std::function<Response(const AiRequest &)> ai;
};

// Routes one request to its handler, driving `editor` (the one open project)
// and the injected `services`. Reads (`get`, `document`, `list`) never mutate;
// every mutation goes through `Editor::apply` so undo/redo works. Returns a
// typed error for a refused edit or a method whose feature is not built.
Response dispatch(const Request &request, genesis::project::Editor &editor,
                  const Services &services);

// Routes one extension-management verb to its `Manager` operation. The seam
// (see `extensions_seam`) hands the inner variant here.
Response dispatch_extensions(const ExtensionsRequest &request,
                             genesis::extensions::Manager &manager);

// The `ExtensionInfo` one host record becomes, for the wire.
ExtensionInfo extension_info_from(const genesis::extensions::ExtensionRecord &record);

// The `Services::extensions` seam over a store manager: closes over `manager`
// so `dispatch` reaches the extension store without linking it into the host.
std::function<Response(const ExtensionsRequest &)>
extensions_seam(std::shared_ptr<genesis::extensions::Manager> manager);

// The `Services::config_export` seam over a store and a settings seam: closes
// over them so `dispatch` can export a profile without linking the store into
// the host. `app_version` is recorded in the profile.
std::function<Response(const ConfigExport &)>
config_export_seam(std::filesystem::path store_root, genesis::extensions::ConfigSeam seam,
                   std::string app_version);

// The `Services::config_import` seam, the import counterpart.
std::function<Response(const ConfigImport &)>
config_import_seam(std::filesystem::path store_root, genesis::extensions::ConfigSeam seam);

// The `Services::extensions_catalog` seam over an injected URL fetch: closes
// over `fetcher` so `dispatch` can list a marketplace catalog without linking
// an HTTP client into the host. The fetcher type is spelled out (a using-alias
// cannot be forward-declared); it matches extensions::CatalogFetcher.
std::function<Response(const ExtensionsCatalog &)>
extensions_catalog_seam(std::function<std::optional<std::string>(const std::string &)> fetcher);

// The `Services::export_presets` value over the render catalogue: the named
// export targets, each with its frame, rate and codec family. Bound in the app
// (the only place the preset source lives) so `export.presets` reads the same
// catalogue the built-in export dialog offers.
std::vector<ExportPresetInfo> export_presets_view();

// The wire state `ai.status` reports for a controller's state/stage pair. The
// controllers spell states "Idle"/"Consent"/"Running"/"Done"/"Cancelled"/
// "Failed" and stages "downloading model"/...; the wire closes over the five
// states idle|downloading|running|done|failed. A "Consent" (waiting for a human
// to approve a download) reads as idle - the API implies consent, so it never
// surfaces a prompt.
std::string ai_status_state(std::string_view controller_state, std::string_view stage);

} // namespace genesis::api
