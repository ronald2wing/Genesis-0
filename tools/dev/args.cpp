// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "tools/dev/args.h"

#include <cstdlib>
#include <optional>

#include "tools/dev/classify.h"

namespace genesis::dev {

namespace {

// Returns the string that follows `--key` as either the next argument or the
// `--key=value` suffix, or nullopt when neither is present. `index` advances
// past a consumed value argument.
std::optional<std::string> flag_value(const std::vector<std::string> &argv, std::size_t &index,
                                      std::string_view key)
{
    const std::string &arg = argv[index];
    if (arg == key) {
        if (index + 1 >= argv.size()) {
            return std::nullopt;
        }
        ++index;
        return argv[index];
    }
    const std::string prefix = std::string(key) + "=";
    if (arg.rfind(prefix, 0) == 0) {
        return arg.substr(prefix.size());
    }
    return std::nullopt;
}

// Parses a non-negative integer, or nullopt.
std::optional<long long> parse_nonneg(std::string_view value)
{
    if (value.empty()) {
        return std::nullopt;
    }
    long long out = 0;
    for (const char c : value) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        out = out * 10 + (c - '0');
    }
    return out;
}

// Splits a comma-separated surface list, trimming each entry.
std::vector<std::string> split_surfaces(std::string_view value)
{
    std::vector<std::string> surfaces;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t comma = value.find(',', start);
        std::string_view entry = value.substr(
                start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
        while (!entry.empty() && (entry.front() == ' ' || entry.front() == '\t')) {
            entry.remove_prefix(1);
        }
        while (!entry.empty() && (entry.back() == ' ' || entry.back() == '\t')) {
            entry.remove_suffix(1);
        }
        if (!entry.empty()) {
            surfaces.emplace_back(entry);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return surfaces;
}

bool known_surface(std::string_view name)
{
    for (const std::string &surface : known_surfaces()) {
        if (surface == name) {
            return true;
        }
    }
    return false;
}

} // namespace

SmokeArgs parse_smoke_args(const std::vector<std::string> &argv)
{
    SmokeArgs args;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const std::string &arg = argv[i];
        if (arg == "--json") {
            args.json = true;
        } else if (arg == "--app" || arg.rfind("--app=", 0) == 0) {
            const auto value = flag_value(argv, i, "--app");
            if (!value || value->empty()) {
                args.ok = false;
                args.error = "--app requires a path";
                return args;
            }
            args.app = *value;
        } else if (arg == "--out" || arg.rfind("--out=", 0) == 0) {
            const auto value = flag_value(argv, i, "--out");
            if (!value || value->empty()) {
                args.ok = false;
                args.error = "--out requires a directory";
                return args;
            }
            args.out = *value;
        } else if (arg == "--seconds" || arg.rfind("--seconds=", 0) == 0) {
            const auto value = flag_value(argv, i, "--seconds");
            const auto parsed = value ? parse_nonneg(*value) : std::nullopt;
            if (!parsed || *parsed <= 0) {
                args.ok = false;
                args.error = "--seconds requires a positive integer (ms)";
                return args;
            }
            args.seconds_ms = static_cast<long>(*parsed);
        } else if (arg == "--max-rss-mb" || arg.rfind("--max-rss-mb=", 0) == 0) {
            const auto value = flag_value(argv, i, "--max-rss-mb");
            const auto parsed = value ? parse_nonneg(*value) : std::nullopt;
            if (!parsed || *parsed <= 0) {
                args.ok = false;
                args.error = "--max-rss-mb requires a positive integer";
                return args;
            }
            args.max_rss_mb = *parsed;
        } else if (arg == "--surfaces" || arg.rfind("--surfaces=", 0) == 0) {
            const auto value = flag_value(argv, i, "--surfaces");
            if (!value || value->empty()) {
                args.ok = false;
                args.error = "--surfaces requires a comma-separated list";
                return args;
            }
            args.surfaces = split_surfaces(*value);
            for (const std::string &surface : args.surfaces) {
                if (!known_surface(surface)) {
                    args.ok = false;
                    args.error = "unknown surface \"" + surface + "\"";
                    return args;
                }
            }
        } else {
            args.ok = false;
            args.error = "unknown option \"" + arg + "\"";
            return args;
        }
    }
    return args;
}

VerifyArgs parse_verify_args(const std::vector<std::string> &argv)
{
    VerifyArgs args;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const std::string &arg = argv[i];
        if (arg == "--json") {
            args.json = true;
        } else if (arg == "--skip-smoke") {
            args.skip_smoke = true;
        } else if (arg == "--strict") {
            args.strict = true;
        } else if (arg == "--build-dir" || arg.rfind("--build-dir=", 0) == 0) {
            const auto value = flag_value(argv, i, "--build-dir");
            if (!value || value->empty()) {
                args.ok = false;
                args.error = "--build-dir requires a directory";
                return args;
            }
            args.build_dir = *value;
        } else {
            args.ok = false;
            args.error = "unknown option \"" + arg + "\"";
            return args;
        }
    }
    return args;
}

SuitesArgs parse_suites_args(const std::vector<std::string> &argv)
{
    SuitesArgs args;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const std::string &arg = argv[i];
        if (arg == "--json") {
            args.json = true;
        } else if (!arg.empty() && arg[0] == '-') {
            args.ok = false;
            args.error = "unknown option \"" + arg + "\"";
            return args;
        } else {
            args.files.push_back(arg);
        }
    }
    return args;
}

FeaturesArgs parse_features_args(const std::vector<std::string> &argv)
{
    FeaturesArgs args;
    for (const std::string &arg : argv) {
        if (arg == "--json") {
            args.json = true;
        } else {
            args.ok = false;
            args.error = "unknown option \"" + arg + "\"";
            return args;
        }
    }
    return args;
}

WhereArgs parse_where_args(const std::vector<std::string> &argv)
{
    WhereArgs args;
    for (const std::string &arg : argv) {
        if (arg == "--json") {
            args.json = true;
        } else if (!arg.empty() && arg[0] == '-') {
            args.ok = false;
            args.error = "unknown option \"" + arg + "\"";
            return args;
        } else {
            args.keywords.push_back(arg);
        }
    }
    return args;
}

} // namespace genesis::dev
