// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Derived from Concat's concat-api/src/message.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "extensions/Sources.h"
#include "project/command/Command.h"
#include "project/model/VideoSettings.h"

namespace genesis::api {

// A helper so an unhandled `Request` alternative fails the build (a
// `static_assert` dependent on the alternative's type) rather than silently
// doing nothing when a method is added without a handler.
namespace detail {
template <typename>
inline constexpr bool dependent_false_v = false;
} // namespace detail

// One template slot and the file that takes it (message.rs `Fill`).
struct Fill
{
    // The placeholder's media id in the template.
    std::string media_id;
    // The file to put there; probed on the way in.
    std::string file;

    bool operator==(const Fill &) const = default;
};

// How an export is rendered, as the request carries it (message.rs
// `ExportSpec`): everything but `output` is optional and defaults to the
// window's sheet defaults. Request-side only - the host never converts this
// into a `render::ExportSpec` itself, because the export runner is not built;
// the shape is carried so the wire contract is complete and a later runner
// reads it unchanged.
struct ExportSpecRequest
{
    // The file to write.
    std::string output;
    // Constant rate factor; lower is better and bigger.
    std::optional<std::uint8_t> crf;
    // The x264 preset name.
    std::optional<std::string> preset;
    // Output width; the timeline's when absent.
    std::optional<std::uint32_t> width;
    // Output height; the timeline's when absent.
    std::optional<std::uint32_t> height;
    // Frame rate numerator; the timeline's when absent.
    std::optional<std::int64_t> rate_num;
    // Frame rate denominator; the timeline's when absent.
    std::optional<std::int64_t> rate_den;
    // "h264", "hevc" or "av1".
    std::optional<std::string> codec;
    // Ten bits a channel.
    std::optional<bool> ten_bit;
    // "limited" or "full".
    std::optional<std::string> color_range;
    // An HDR timeline written as HDR rather than tone-mapped to SDR.
    std::optional<bool> hdr;

    bool operator==(const ExportSpecRequest &) const = default;
};

// What a caller asks for. One struct per method, named `area.verb`. Every
// method that touches a project carries its folder in `path` for wire
// fidelity; the engine-free, single-Editor host routes them to the one open
// `Editor` and treats `path` as the document file itself (see Api.h).

// The API's version, the build behind it, and what that build serves.
struct Version
{
    bool operator==(const Version &) const = default;
};

// Creates a project folder under `location`, named `name`, and opens it.
struct ProjectCreate
{
    std::string location;
    std::string name;
    // Frame and rate; 1080p at 30 when absent.
    std::optional<genesis::project::VideoSettings> video;

    bool operator==(const ProjectCreate &) const = default;
};

// Opens a project folder.
struct ProjectOpen
{
    std::string path;

    bool operator==(const ProjectOpen &) const = default;
};

// Closes an open project, saving first when asked.
struct ProjectClose
{
    std::string path;
    bool save = false;

    bool operator==(const ProjectClose &) const = default;
};

// The projects this machine opened most recently, newest first.
struct ProjectList
{
    bool operator==(const ProjectList &) const = default;
};

// The state of an open project.
struct ProjectGet
{
    std::string path;

    bool operator==(const ProjectGet &) const = default;
};

// The document exactly as a save would write it.
struct ProjectDocument
{
    std::string path;

    bool operator==(const ProjectDocument &) const = default;
};

// Writes the document to the project folder.
struct ProjectSave
{
    std::string path;
    // A new name for the project, when renaming.
    std::optional<std::string> name;

    bool operator==(const ProjectSave &) const = default;
};

// Sets the active timeline's frame and rate, as an undoable edit.
struct ProjectSetVideo
{
    std::string path;
    genesis::project::VideoSettings video;

    bool operator==(const ProjectSetVideo &) const = default;
};

// Applies one edit command.
struct EditApply
{
    std::string path;
    genesis::project::Command command;

    bool operator==(const EditApply &) const = default;
};

// Steps the history back one edit.
struct EditUndo
{
    std::string path;

    bool operator==(const EditUndo &) const = default;
};

// Steps the history forward one edit.
struct EditRedo
{
    std::string path;

    bool operator==(const EditRedo &) const = default;
};

// The currently selected clip, if any.
struct EditSelection
{
    bool operator==(const EditSelection &) const = default;
};

// Reports what is inside a media file without touching any project.
struct MediaProbe
{
    std::string path;

    bool operator==(const MediaProbe &) const = default;
};

// Probes a file and adds it to a project's bin.
struct MediaImport
{
    std::string path;
    std::string file;

    bool operator==(const MediaImport &) const = default;
};

// Every media item in the open project's bin.
struct MediaList
{
    bool operator==(const MediaList &) const = default;
};

// Every effect package the build knows.
struct CatalogueList
{
    // Restrict to one kind: "effect", "filter", "audio", "transition" or
    // "generator".
    std::optional<std::string> kind;

    bool operator==(const CatalogueList &) const = default;
};

// The title-template library (the in-house SVG artwork and its text slots).
struct TemplateList
{
    bool operator==(const TemplateList &) const = default;
};

// Fills one title template's text slots with caller text, returning the
// per-slot (text, style) pairs an `edit.apply` `addTextClip` can consume.
struct TemplateFill
{
    // The template's name: the SVG basename without the .svg extension
    // (`template` is a C++ keyword, hence the trailing underscore).
    std::string template_;
    // One text per slot, in slot order; a missing entry keeps the slot's
    // placeholder, an empty entry blanks it.
    std::vector<std::string> texts;

    bool operator==(const TemplateFill &) const = default;
};

// Makes a project from a template with every slot filled, and opens it.
struct TemplateInstantiate
{
    // The template bundle folder (message.rs spells it `template`, which is
    // a C++ keyword).
    std::string template_;
    std::string location;
    std::string name;
    std::vector<Fill> fills;

    bool operator==(const TemplateInstantiate &) const = default;
};

// Packs an open project into a new template bundle.
struct TemplateSave
{
    std::string path;
    std::string name;

    bool operator==(const TemplateSave &) const = default;
};

// Renders an open project's active timeline to a file.
struct ExportRun
{
    std::string path;
    ExportSpecRequest spec;

    bool operator==(const ExportRun &) const = default;
};

// Stops a running export at its next frame.
struct ExportCancel
{
    std::string job;

    bool operator==(const ExportCancel &) const = default;
};

// Reports a running (or finished) export's state.
struct ExportStatus
{
    std::string job;

    bool operator==(const ExportStatus &) const = default;
};

// Composites the true frame at one instant.
struct PreviewFrame
{
    std::string path;
    // The timeline instant, in seconds.
    double time = 0.0;
    // The file to write; absent means the picture comes back inline.
    std::optional<std::string> output;
    // Frame width; the timeline's when absent.
    std::optional<std::uint32_t> width;
    // Frame height; the timeline's when absent.
    std::optional<std::uint32_t> height;

    bool operator==(const PreviewFrame &) const = default;
};

// The preview playhead position, in seconds.
struct PreviewTime
{
    bool operator==(const PreviewTime &) const = default;
};

// The current frame's scope data, one scope at a time.
struct ScopesSnapshot
{
    // Which scope to return: "histogram", "waveform" or "vectorscope".
    // Anything else (or empty) reads as "histogram".
    std::string kind;

    bool operator==(const ScopesSnapshot &) const = default;
};

// The hardware probe's read-only status.
struct GpuStatus
{
    bool operator==(const GpuStatus &) const = default;
};

// Lists the installed native-extension store.
struct ExtensionsList
{
    bool operator==(const ExtensionsList &) const = default;
};

// Removes the installed native extension `id`.
struct ExtensionsRemove
{
    std::string id;

    bool operator==(const ExtensionsRemove &) const = default;
};

// Enables the installed native extension `id`.
struct ExtensionsEnable
{
    std::string id;

    bool operator==(const ExtensionsEnable &) const = default;
};

// Disables the installed native extension `id` (and its dependents).
struct ExtensionsDisable
{
    std::string id;

    bool operator==(const ExtensionsDisable &) const = default;
};

// Restores a removed native extension `id`.
struct ExtensionsRestore
{
    std::string id;

    bool operator==(const ExtensionsRestore &) const = default;
};

// Installs an extension from a source: a directory path (the string `source`)
// or an object `{type: "dir"|"git", url, ref?, subdir?}` naming a directory or
// a git repository. `origin` is the origin name
// ("builtin"/"curated"/"developer"/"user"); absent reads as "developer"
// (consent-gated). The install is refused under the same trust rules as every
// other install path.
struct ExtensionsInstall
{
    extensions::InstallSource source;
    std::optional<std::string> origin;

    bool operator==(const ExtensionsInstall &) const = default;
};

// Exports the machine's portable config (settings plus installed extensions)
// as a profile document, returned inline.
struct ConfigExport
{
    bool operator==(const ConfigExport &) const = default;
};

// Imports a config profile (exactly as `config.export` produced it) onto this
// machine, applying its settings and installing its extensions per the trust
// rules. The profile rides inline rather than by path, so the wire carries the
// whole document and no shared filesystem is assumed.
struct ConfigImport
{
    nlohmann::json profile;

    bool operator==(const ConfigImport &) const = default;
};

// Fetches and lists a marketplace catalog (see extensions/Catalog.h). The url
// names the catalog document; the reply is the parsed entries, one per
// installable extension.
struct ExtensionsCatalog
{
    std::string url;

    bool operator==(const ExtensionsCatalog &) const = default;
};

// The extension-management requests, one alternative per verb. Wrapped in a
// struct (not a bare alias) for the same reason `Request` and `Command` are:
// `dispatch` routes on the outer `Request`, and the extensions seam re-visits
// this inner variant to reach the specific verb.
struct ExtensionsRequest
{
    std::variant<ExtensionsList, ExtensionsRemove, ExtensionsEnable, ExtensionsDisable,
                 ExtensionsRestore, ExtensionsInstall>
            value;

    bool operator==(const ExtensionsRequest &) const = default;

    template <typename T>
        requires std::is_constructible_v<decltype(value), T &&>
    ExtensionsRequest(T &&alternative) : value(std::forward<T>(alternative))
    {
    }
};

namespace detail {
// True when `T` is one of the extension-management verbs, so the outer
// `method_name` / `dispatch` visitors can hand it to the inner visit without a
// `static_assert` on the wrapper type itself.
template <typename T>
inline constexpr bool is_extensions_request_v =
        std::is_same_v<T, ExtensionsList> || std::is_same_v<T, ExtensionsRemove>
        || std::is_same_v<T, ExtensionsEnable> || std::is_same_v<T, ExtensionsDisable>
        || std::is_same_v<T, ExtensionsRestore> || std::is_same_v<T, ExtensionsInstall>;
} // namespace detail

// The closed request set, one alternative per method. A struct wrapping the
// variant (not a bare alias) for the same reason `Command` is: no bare alias
// can name itself, and the converting constructor lets `Request r =
// ProjectOpen{...}` read like the Rust enum it ports.
struct Request
{
    std::variant<Version, ProjectCreate, ProjectOpen, ProjectClose, ProjectList, ProjectGet,
                 ProjectDocument, ProjectSave, ProjectSetVideo, EditApply, EditUndo, EditRedo,
                 EditSelection, MediaProbe, MediaImport, MediaList, CatalogueList, TemplateList,
                 TemplateFill, TemplateInstantiate, TemplateSave, ExportRun, ExportCancel,
                 ExportStatus, PreviewFrame, PreviewTime, ScopesSnapshot, GpuStatus,
                 ExtensionsRequest, ConfigExport, ConfigImport, ExtensionsCatalog>
            value;

    bool operator==(const Request &) const = default;

    template <typename T>
        requires std::is_constructible_v<decltype(value), T &&>
    Request(T &&alternative) : value(std::forward<T>(alternative))
    {
    }
};

// The wire method name of a request, e.g. "project.create". A caller uses it
// to tag the request on a transport; `dispatch` does not need it.
std::string_view method_name(const Request &request);

} // namespace genesis::api
