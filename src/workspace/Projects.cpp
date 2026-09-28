// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "workspace/Projects.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "core/JsonGuard.h"
#include "project/Document.h"
#include "project/model/VideoSettings.h"
#include "workspace/ProjectClaim.h"

namespace genesis::workspace {

namespace core = genesis::core;

namespace {

// The document filename. A fresh project is written with it, and an existing
// project on disk keeps opening under the same name.
constexpr const char *MANIFEST_FILENAME = "concat.json";

// Milliseconds since the epoch; a clock behind the epoch reads as zero
// rather than wrapping a negative into a huge unsigned.
std::uint64_t now_millis()
{
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch);
    return millis.count() < 0 ? 0 : static_cast<std::uint64_t>(millis.count());
}

// Paths compare case-insensitively, because Windows does: the same project
// reopened with the drive letter in another case is not a new entry.
bool same_path(std::string_view left, std::string_view right)
{
    return std::equal(left.begin(), left.end(), right.begin(), right.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a))
                == std::tolower(static_cast<unsigned char>(b));
    });
}

// A non-negative integer field, or the fallback when absent or not an
// integer. A hand-edited manifest still opens at a size that is a size.
std::uint32_t take_u32(const nlohmann::json &object, const char *key, std::uint32_t fallback)
{
    const auto it = object.find(key);
    if (it == object.end() || (!it->is_number_integer() && !it->is_number_unsigned())) {
        return fallback;
    }
    if (it->is_number_unsigned()) {
        const std::uint64_t value = it->get<std::uint64_t>();
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            return fallback;
        }
        return static_cast<std::uint32_t>(value);
    }
    const std::int64_t value = it->get<std::int64_t>();
    if (value < 0 || value > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
        return fallback;
    }
    return static_cast<std::uint32_t>(value);
}

std::int64_t take_i64(const nlohmann::json &object, const char *key, std::int64_t fallback)
{
    const auto it = object.find(key);
    if (it == object.end() || (!it->is_number_integer() && !it->is_number_unsigned())) {
        return fallback;
    }
    if (it->is_number_unsigned()) {
        const std::uint64_t value = it->get<std::uint64_t>();
        if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return fallback;
        }
        return static_cast<std::int64_t>(value);
    }
    return it->get<std::int64_t>();
}

std::string take_string(const nlohmann::json &object, const char *key, std::string fallback = { })
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        return fallback;
    }
    return it->get<std::string>();
}

bool take_bool(const nlohmann::json &object, const char *key, bool fallback = false)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_boolean()) {
        return fallback;
    }
    return it->get<bool>();
}

// One recents entry, read tolerantly: a missing field keeps its default, an
// entry without a string path is skipped by the caller.
ProjectInfo read_entry(const nlohmann::json &entry)
{
    ProjectInfo info;
    info.path = take_string(entry, "path");
    info.name = take_string(entry, "name");
    info.width = take_u32(entry, "width", 1920);
    info.height = take_u32(entry, "height", 1080);
    info.rate_num = take_i64(entry, "rateNum", 30);
    info.rate_den = take_i64(entry, "rateDen", 1);
    const std::int64_t opened = take_i64(entry, "openedAt", 0);
    info.opened_at = opened < 0 ? 0 : static_cast<std::uint64_t>(opened);
    info.pinned = take_bool(entry, "pinned");
    return info;
}

nlohmann::json write_entry(const ProjectInfo &info)
{
    nlohmann::json entry = nlohmann::json::object();
    entry["path"] = info.path;
    entry["name"] = info.name;
    entry["width"] = info.width;
    entry["height"] = info.height;
    entry["rateNum"] = info.rate_num;
    entry["rateDen"] = info.rate_den;
    entry["openedAt"] = info.opened_at;
    entry["pinned"] = info.pinned;
    return entry;
}

// A missing or corrupt list is not an error worth showing anyone; it just
// means there is no history yet.
std::vector<ProjectInfo> read_recents(const std::filesystem::path &config)
{
    std::ifstream in(config / RECENTS_FILE);
    if (!in) {
        return { };
    }
    std::string error;
    const std::optional<nlohmann::json> parsed = core::parse_document_json(in, error);
    if (!parsed || !parsed->is_array()) {
        return { };
    }
    const nlohmann::json &document = *parsed;
    std::vector<ProjectInfo> entries;
    for (const nlohmann::json &entry : document) {
        if (!entry.is_object() || !entry["path"].is_string()) {
            continue;
        }
        entries.push_back(read_entry(entry));
    }
    return entries;
}

void write_recents(const std::filesystem::path &config, const std::vector<ProjectInfo> &entries)
{
    std::error_code error;
    std::filesystem::create_directories(config, error);
    if (error) {
        return;
    }
    nlohmann::json document = nlohmann::json::array();
    for (const ProjectInfo &entry : entries) {
        document.push_back(write_entry(entry));
    }
    std::ofstream out(config / RECENTS_FILE);
    if (!out) {
        return;
    }
    out << document.dump(2) << '\n';
}

} // namespace

std::filesystem::path manifest_path(const std::filesystem::path &root)
{
    return root / MANIFEST_FILENAME;
}

bool is_project(const std::filesystem::path &root)
{
    return std::filesystem::is_regular_file(manifest_path(root));
}

void add_recents(const std::filesystem::path &config, ProjectInfo project)
{
    std::vector<ProjectInfo> entries = read_recents(config);
    std::erase_if(entries, [&project](const ProjectInfo &entry) {
        return same_path(entry.path, project.path);
    });
    entries.insert(entries.begin(), std::move(project));
    if (entries.size() > MAX_RECENTS) {
        entries.resize(MAX_RECENTS);
    }
    write_recents(config, entries);
}

std::vector<ProjectInfo> list_recents(const std::filesystem::path &config)
{
    std::vector<ProjectInfo> entries = read_recents(config);
    std::erase_if(entries, [](const ProjectInfo &entry) {
        return !is_project(std::filesystem::path(entry.path));
    });
    return entries;
}

void remove_recents(const std::filesystem::path &config, std::string_view path)
{
    std::vector<ProjectInfo> entries = read_recents(config);
    std::erase_if(entries,
                  [path](const ProjectInfo &entry) { return same_path(entry.path, path); });
    write_recents(config, entries);
}

void pin_recents(const std::filesystem::path &config, std::string_view path, bool pinned)
{
    std::vector<ProjectInfo> entries = read_recents(config);
    for (ProjectInfo &entry : entries) {
        if (same_path(entry.path, path)) {
            entry.pinned = pinned;
        }
    }
    write_recents(config, entries);
}

std::string folder_name(std::string_view name)
{
    std::string cleaned;
    cleaned.reserve(name.size());
    for (char character : name) {
        const unsigned char byte = static_cast<unsigned char>(character);
        // Windows refuses names ending in a dot or a space; control and
        // reserved characters become dashes so no name is ever empty.
        if (character == '<' || character == '>' || character == ':' || character == '"'
            || character == '/' || character == '\\' || character == '|' || character == '?'
            || character == '*' || std::iscntrl(byte)) {
            cleaned.push_back('-');
        } else {
            cleaned.push_back(character);
        }
    }
    const auto first = cleaned.find_first_not_of(" \t\n\r\f\v");
    if (first == std::string::npos) {
        return "Untitled project";
    }
    const auto last = cleaned.find_last_not_of(" \t\n\r\f\v.");
    if (last == std::string::npos || last < first) {
        return "Untitled project";
    }
    return cleaned.substr(first, last - first + 1);
}

std::expected<ProjectInfo, std::string> create_project(const std::filesystem::path &location,
                                                       std::string_view name,
                                                       const project::VideoSettings &video)
{
    const std::filesystem::path root = location / folder_name(name);

    // Any name counts as an existing project, so a folder with a manifest is
    // just as safe from being clobbered as one without.
    if (is_project(root)) {
        return std::unexpected{ std::string{ "a project already exists at " + root.string() } };
    }

    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) {
        return std::unexpected{ std::string{ "could not create " + root.string() } };
    }

    // Settings only: a fresh project has no edit yet. The full document -
    // timelines included - is written by the session's save from the first
    // change.
    nlohmann::json document = nlohmann::json::object();
    document["version"] = project::DOCUMENT_VERSION;
    document["name"] = std::string(name);
    document["video"] = nlohmann::json::object({ { "width", video.width },
                                                 { "height", video.height },
                                                 { "rateNum", video.rate_num },
                                                 { "rateDen", video.rate_den } });

    std::ofstream out(root / MANIFEST_FILENAME);
    if (!out) {
        return std::unexpected{ std::string{ "could not write the manifest" } };
    }
    out << document.dump(2) << '\n';
    if (!out) {
        return std::unexpected{ std::string{ "could not write the manifest" } };
    }

    ProjectInfo info;
    info.path = root.string();
    info.name = std::string(name);
    info.width = video.width;
    info.height = video.height;
    info.rate_num = video.rate_num;
    info.rate_den = video.rate_den;
    info.opened_at = now_millis();
    return info;
}

std::expected<ProjectInfo, std::string> open_project(const std::filesystem::path &root)
{
    const std::filesystem::path manifest = manifest_path(root);
    std::ifstream in(manifest);
    if (!in) {
        return std::unexpected{ std::string{ "could not read " + manifest.string() } };
    }
    std::string error;
    const std::optional<nlohmann::json> parsed = core::parse_document_json(in, error);
    if (!parsed) {
        return std::unexpected{ std::string{ manifest.string() + ": " + error } };
    }
    const nlohmann::json &document = *parsed;
    if (!document.is_object()) {
        return std::unexpected{ std::string{ manifest.string() + " is not a project" } };
    }

    ProjectInfo info;
    info.path = root.string();
    info.name = document.value("name", std::string{ });
    const nlohmann::json video = document.contains("video") && document["video"].is_object()
            ? document["video"]
            : nlohmann::json::object();
    info.width = take_u32(video, "width", 1920);
    info.height = take_u32(video, "height", 1080);
    info.rate_num = take_i64(video, "rateNum", 30);
    info.rate_den = take_i64(video, "rateDen", 1);
    if (info.rate_den == 0) {
        return std::unexpected{ std::string{ manifest.string() + " has an invalid frame rate" } };
    }

    // The manifest reads clean, so claim the folder before returning it as
    // open. The claim is taken here - after a successful read, before any
    // caller loads the document - so a failed open leaves nothing claimed and
    // the previous project's claim stays intact.
    if (const std::optional<std::string> refusal = claim_project(root)) {
        return std::unexpected{ std::string{ *refusal } };
    }
    info.opened_at = now_millis();
    return info;
}

} // namespace genesis::workspace
