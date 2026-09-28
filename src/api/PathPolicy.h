// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <variant>
#include <vector>

#include "api/Api.h"
#include "api/Requests.h"

namespace genesis::api {

// How a named path is used by a request. The two are confined independently:
// a confined server refuses a path that resolves - symlinks followed, any `..`
// refused - to a point outside every allowlisted root for that use. Reads and
// writes keep separate root sets, so a server can refuse writes outside its
// roots while still probing/opening media anywhere its owner can read (the
// posture of the reference editor's host crate).
enum class PathUse { Read, Write };

// A remote-surface path policy: an allowlist of canonical roots that confines
// which filesystem paths a request may name. It answers "a token-gated server
// can be asked to read/write anywhere the process can": an *active* policy
// (one with roots for the use) refuses a named path that escapes the roots; an
// *inactive* policy (empty roots) confines nothing, which is the in-process
// app/CLI posture.
class PathPolicy
{
public:
    // Adds `root` to the allowlist for `use`. The root is weakly canonicalized
    // (symlinks resolved where the path exists, the non-existent tail kept), so
    // a root names a real location; on a canonicalization failure the raw path
    // is kept and matches only its exact spelling (fail closed).
    void allow(const std::filesystem::path &root, PathUse use)
    {
        std::error_code ec;
        std::filesystem::path resolved = std::filesystem::weakly_canonical(root, ec);
        if (ec) {
            resolved = root;
        }
        (use == PathUse::Write ? write_roots_ : read_roots_).push_back(std::move(resolved));
    }

    // Whether `path` may be used for `use`. Nullopt = allowed. Otherwise the
    // refusal names the path, the allowed roots, and why it was refused.
    std::optional<ApiError> check(std::string_view path, PathUse use) const
    {
        const std::vector<std::filesystem::path> &roots =
                use == PathUse::Write ? write_roots_ : read_roots_;
        if (roots.empty()) {
            return std::nullopt;
        }
        const auto refusal = [&](std::string_view why) {
            std::string allowed;
            for (const std::filesystem::path &root : roots) {
                if (!allowed.empty()) {
                    allowed += ", ";
                }
                allowed += root.string();
            }
            return ApiError{ ErrorCode::Refused,
                             std::string(path) + " is outside where this API "
                                     + (use == PathUse::Write ? "writes" : "reads") + " (" + allowed
                                     + "): " + std::string(why) };
        };
        // A `..` past the existing part of the path cannot be resolved and
        // would walk out of a root on paper while staying under it here; a
        // caller has no legitimate need for one.
        if (has_parent_dir(path)) {
            return refusal("a '..' component cannot be confined");
        }
        std::error_code ec;
        const std::filesystem::path target =
                std::filesystem::weakly_canonical(std::filesystem::path(std::string(path)), ec);
        if (ec) {
            return refusal("the path cannot be resolved");
        }
        for (const std::filesystem::path &root : roots) {
            if (is_within(target, root)) {
                return std::nullopt;
            }
        }
        return refusal("it resolves outside the allowed roots");
    }

private:
    // Whether `path` contains a literal `..` component.
    static bool has_parent_dir(std::string_view path)
    {
        for (const std::filesystem::path &part : std::filesystem::path(path)) {
            if (part == std::filesystem::path("..")) {
                return true;
            }
        }
        return false;
    }

    // Whether `path` is `root` or lies under it, compared component-wise so a
    // sibling that shares a string prefix (`/a/bc` vs `/a/b`) is not "within".
    static bool is_within(const std::filesystem::path &path, const std::filesystem::path &root)
    {
        auto p = path.begin();
        auto r = root.begin();
        for (; r != root.end(); ++r, ++p) {
            if (p == path.end() || *p != *r) {
                return false;
            }
        }
        return true;
    }

    std::vector<std::filesystem::path> write_roots_;
    std::vector<std::filesystem::path> read_roots_;
};

// Whether a request's named paths all pass `policy`, else the refusal. Only the
// methods that actually name a filesystem path are checked: `project.open`
// reads its document, `project.save` and `project.close` (when saving) write
// it, `media.probe`/`media.import` read a media file, and `export.run` writes
// an output file. Every other method names no path and passes through
// untouched. A request whose path is empty (an inline reply or a no-op) is not
// checked.
inline std::optional<ApiError> check_request_paths(const Request &request, const PathPolicy &policy)
{
    return std::visit(
            [&](const auto &alternative) -> std::optional<ApiError> {
                using T = std::decay_t<decltype(alternative)>;
                if constexpr (std::is_same_v<T, ProjectOpen>) {
                    return policy.check(alternative.path, PathUse::Read);
                } else if constexpr (std::is_same_v<T, ProjectSave>) {
                    return policy.check(alternative.path, PathUse::Write);
                } else if constexpr (std::is_same_v<T, ProjectClose>) {
                    return alternative.save ? policy.check(alternative.path, PathUse::Write)
                                            : std::nullopt;
                } else if constexpr (std::is_same_v<T, MediaProbe>) {
                    return policy.check(alternative.path, PathUse::Read);
                } else if constexpr (std::is_same_v<T, MediaImport>) {
                    return policy.check(alternative.file, PathUse::Read);
                } else if constexpr (std::is_same_v<T, ExportRun>) {
                    return alternative.spec.output.empty()
                            ? std::nullopt
                            : policy.check(alternative.spec.output, PathUse::Write);
                } else {
                    return std::nullopt;
                }
            },
            request.value);
}

// The default confinement for a token-gated server (`serve` / `serve --grpc`):
// writes are confined to the user's home directory plus the process working
// directory - the two places a headless server legitimately writes projects
// and exports - widened by each operator-named `extra_write_roots` (a
// repeatable `--allow-root`; name `/` to allow anywhere). Reads stay
// unconfined, matching the reference editor's host crate: a server may probe/open media
// anywhere its owner can read, but it cannot be asked to write outside its
// allowed roots.
inline PathPolicy
server_path_policy(const std::vector<std::filesystem::path> &extra_write_roots = { })
{
    PathPolicy policy;
    const auto add = [&policy](const std::filesystem::path &root) {
        if (!root.empty()) {
            policy.allow(root, PathUse::Write);
        }
    };
    if (const char *home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        add(home);
    } else if (const char *profile = std::getenv("USERPROFILE");
               profile != nullptr && *profile != '\0') {
        add(profile);
    }
    std::error_code ec;
    add(std::filesystem::current_path(ec));
    for (const std::filesystem::path &root : extra_write_roots) {
        add(root);
    }
    return policy;
}

} // namespace genesis::api
