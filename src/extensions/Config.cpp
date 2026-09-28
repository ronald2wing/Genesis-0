// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/Config.h"

#include <cstdint>
#include <exception>
#include <map>
#include <string>
#include <utility>

#include "extensions/Install.h"
#include "extensions/Origins.h"
#include "extensions/SourceInstall.h"

namespace genesis::extensions {

namespace {

// The numerically highest version key of an id's version -> root map. Version
// keys are strings (`installed_packs`), so "10" must outrank "9"; parsing to
// an integer and taking the max is the same resolution the host's scan uses.
std::uint32_t highest_version(const std::map<std::string, std::filesystem::path> &versions)
{
    std::uint32_t best = 0;
    for (const auto &[version, root] : versions) {
        (void)root;
        try {
            const std::uint32_t parsed = static_cast<std::uint32_t>(std::stoul(version));
            if (parsed > best) {
                best = parsed;
            }
        } catch (const std::exception &) {
            // A non-numeric version key is skipped; the numeric ones win.
        }
    }
    return best;
}

// A source record as its profile JSON, or null when the record is absent.
nlohmann::json source_json(const std::optional<InstallSource> &source)
{
    if (!source) {
        return nullptr;
    }
    nlohmann::json out = nlohmann::json::object();
    out["type"] = source_type_name(source->type);
    if (source->type != SourceType::Builtin) {
        out["url"] = source->url;
    }
    if (source->type == SourceType::Git && !source->ref.empty()) {
        out["ref"] = source->ref;
    }
    if (source->type == SourceType::Git && !source->subdir.empty()) {
        out["subdir"] = source->subdir;
    }
    return out;
}

// A profile extension entry's source record, or nullopt when absent or
// malformed.
std::optional<InstallSource> source_from(const nlohmann::json &entry)
{
    const auto it = entry.find("source");
    if (it == entry.end() || it->is_null()) {
        return std::nullopt;
    }
    if (!it->is_object()) {
        return std::nullopt;
    }
    InstallSource source;
    const auto type_it = it->find("type");
    if (type_it != it->end() && type_it->is_string()) {
        source.type = parse_source_type(type_it->get<std::string>()).value_or(SourceType::Dir);
    }
    const auto url_it = it->find("url");
    if (url_it != it->end() && url_it->is_string()) {
        source.url = url_it->get<std::string>();
    }
    const auto ref_it = it->find("ref");
    if (ref_it != it->end() && ref_it->is_string()) {
        source.ref = ref_it->get<std::string>();
    }
    const auto subdir_it = it->find("subdir");
    if (subdir_it != it->end() && subdir_it->is_string()) {
        source.subdir = subdir_it->get<std::string>();
    }
    return source;
}

// The origin spelled in a profile entry, or Developer when absent or unknown
// (the conservative consent-gated default).
Origin entry_origin(const nlohmann::json &entry)
{
    const auto it = entry.find("origin");
    if (it == entry.end() || !it->is_string()) {
        return Origin::Developer;
    }
    return parse_origin(it->get<std::string>());
}

} // namespace

nlohmann::json export_profile(const std::filesystem::path &store_root, const ConfigSeam &seam,
                              std::string_view app_version)
{
    const std::map<std::string, std::map<std::string, std::filesystem::path>> installed =
            installed_packs(store_root);
    const std::map<std::string, Origin> origins = read_origins(store_root);
    const std::map<std::string, InstallSource> sources = read_sources(store_root);

    nlohmann::json settings = nlohmann::json::object();
    if (seam.get_proxies_enabled) {
        if (const auto value = seam.get_proxies_enabled()) {
            settings["proxiesEnabled"] = *value;
        }
    }
    if (seam.get_decode_preference) {
        if (const auto value = seam.get_decode_preference()) {
            settings["decodePreference"] = *value;
        }
    }
    if (seam.get_scopes_interval_ms) {
        if (const auto value = seam.get_scopes_interval_ms()) {
            settings["scopesIntervalMs"] = *value;
        }
    }
    if (seam.get_feed_url) {
        if (const auto value = seam.get_feed_url()) {
            settings["feedUrl"] = *value;
        }
    }

    nlohmann::json extensions = nlohmann::json::array();
    for (const auto &[id, versions] : installed) {
        const std::uint32_t version = highest_version(versions);

        nlohmann::json entry = nlohmann::json::object();
        entry["id"] = id;
        entry["version"] = version;
        const auto origin_it = origins.find(id);
        entry["origin"] =
                origin_name(origin_it != origins.end() ? origin_it->second : Origin::Developer);
        const auto source_it = sources.find(id);
        entry["source"] = source_json(source_it != sources.end()
                                              ? std::optional<InstallSource>(source_it->second)
                                              : std::nullopt);
        extensions.push_back(std::move(entry));
    }

    return nlohmann::json{ { "appVersion", app_version },
                           { "settings", std::move(settings) },
                           { "extensions", std::move(extensions) } };
}

std::optional<ImportReport> import_profile(const nlohmann::json &profile,
                                           const std::filesystem::path &store_root,
                                           const ConfigSeam &seam)
{
    if (!profile.is_object()) {
        return std::nullopt;
    }

    ImportReport report;

    // Settings: apply each present field through its setter, or skip it when
    // this host has no setter wired (the Qt-free CLI wires none). A field of
    // the wrong JSON type is ignored rather than forced.
    const nlohmann::json settings = profile.value("settings", nlohmann::json::object());
    if (const auto it = settings.find("proxiesEnabled"); it != settings.end() && it->is_boolean()) {
        if (seam.set_proxies_enabled) {
            seam.set_proxies_enabled(it->get<bool>());
            report.settings.push_back("applied proxiesEnabled");
        } else {
            report.settings.push_back("skipped proxiesEnabled: no setter wired");
        }
    }
    if (const auto it = settings.find("decodePreference");
        it != settings.end() && it->is_number_integer()) {
        if (seam.set_decode_preference) {
            seam.set_decode_preference(static_cast<int>(it->get<std::int64_t>()));
            report.settings.push_back("applied decodePreference");
        } else {
            report.settings.push_back("skipped decodePreference: no setter wired");
        }
    }
    if (const auto it = settings.find("scopesIntervalMs");
        it != settings.end() && it->is_number_integer()) {
        if (seam.set_scopes_interval_ms) {
            seam.set_scopes_interval_ms(static_cast<int>(it->get<std::int64_t>()));
            report.settings.push_back("applied scopesIntervalMs");
        } else {
            report.settings.push_back("skipped scopesIntervalMs: no setter wired");
        }
    }
    if (const auto it = settings.find("feedUrl"); it != settings.end() && it->is_string()) {
        if (seam.set_feed_url) {
            seam.set_feed_url(it->get<std::string>());
            report.settings.push_back("applied feedUrl");
        } else {
            report.settings.push_back("skipped feedUrl: no setter wired");
        }
    }

    // Extensions: never elevate trust. An already-installed id is left alone;
    // a Developer origin is re-installed from its recorded source (dir or git).
    // A User origin is refused; a trusted origin (Builtin/Curated) without a
    // trusted source is skipped, because the profile alone is not proof.
    const auto installed = installed_packs(store_root);
    const auto extensions_it = profile.find("extensions");
    if (extensions_it != profile.end() && extensions_it->is_array()) {
        for (const nlohmann::json &entry : *extensions_it) {
            if (!entry.is_object()) {
                continue;
            }
            const auto id_it = entry.find("id");
            if (id_it == entry.end() || !id_it->is_string()) {
                continue;
            }
            const std::string id = id_it->get<std::string>();

            if (installed.count(id) != 0) {
                report.extensions.push_back({ id, "skipped", "already installed" });
                continue;
            }
            const std::optional<InstallSource> source = source_from(entry);
            if (!source) {
                report.extensions.push_back({ id, "skipped", "no install source recorded" });
                continue;
            }
            if (source->type == SourceType::Builtin) {
                report.extensions.push_back({ id, "skipped", "re-seeds on startup" });
                continue;
            }

            const Origin origin = entry_origin(entry);
            if (origin == Origin::User) {
                report.extensions.push_back({ id, "failed", "not installable (user origin)" });
                continue;
            }
            if (origin == Origin::Curated || origin == Origin::Builtin) {
                report.extensions.push_back(
                        { id, "skipped", "trusted origin without a trusted source" });
                continue;
            }

            // Developer: re-install from the recorded source (dir or git). The
            // install records the same source, so a later export stays
            // reproducible.
            const InstallResult result =
                    install_from_source(*source, store_root, Origin::Developer);
            if (!result.ok()) {
                std::string reason;
                for (const std::string &problem : result.problems) {
                    if (!reason.empty()) {
                        reason += "; ";
                    }
                    reason += problem;
                }
                report.extensions.push_back({ id, "failed", std::move(reason) });
                continue;
            }
            report.extensions.push_back({ id, "installed", { } });
        }
    }

    return report;
}

} // namespace genesis::extensions
