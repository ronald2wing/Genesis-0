// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/ExtensionHost.h"

#include <limits>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "extensions/Consent.h"
#include "extensions/DisabledList.h"
#include "extensions/Install.h"
#include "extensions/Origins.h"
#include "extensions/Trust.h"

namespace genesis::extensions {

namespace {

// The host table an extension receives: the app's bridge, with the log sink
// re-prefixed so a shared sink still names the extension that spoke.
NativeExtension::Host host_for(const NativeExtension::Host &shared, const std::string &id)
{
    NativeExtension::Host host = shared;
    if (host.log) {
        host.log = [sink = host.log, id](const std::string &message) {
            sink("[ext:" + id + "] " + message);
        };
    }
    return host;
}

// The version directory name, as a number. A non-numeric name (a store someone
// edited by hand) counts as 0 so it never wins over a real version.
std::uint32_t parse_version(const std::string &text)
{
    try {
        const unsigned long value = std::stoul(text);
        return value > static_cast<unsigned long>(std::numeric_limits<std::uint32_t>::max())
                ? std::numeric_limits<std::uint32_t>::max()
                : static_cast<std::uint32_t>(value);
    } catch (...) {
        return 0;
    }
}

// The characters that pass through a file URL un-encoded: alphanumerics and
// the RFC 3986 sub-delims and path characters the QML loader reads as-is.
bool is_unreserved(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/'
            || c == '-' || c == '.' || c == '_' || c == '~';
}

char hex_digit(unsigned int value)
{
    return "0123456789ABCDEF"[value & 0xF];
}

// The `file://` URL of `root/<qml>`: the absolute path with every character
// outside the unreserved set percent-encoded as `%XX`. The loader opens this
// URL; the host computes it so the Qt-free scan can be tested in isolation.
std::string file_url(const std::filesystem::path &root, const std::string &qml)
{
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(root / qml, ec);
    const std::string path = ec ? (root / qml).string() : absolute.string();

    std::string encoded;
    encoded.reserve(path.size());
    for (const unsigned char c : path) {
        if (is_unreserved(c)) {
            encoded += static_cast<char>(c);
        } else {
            encoded += '%';
            encoded += hex_digit(static_cast<unsigned int>(c) >> 4);
            encoded += hex_digit(static_cast<unsigned int>(c) & 0xF);
        }
    }
    return "file://" + encoded;
}

// The identity fields every record carries, filled from a parsed manifest.
ExtensionRecord base_record(const NativeManifest &manifest, const std::filesystem::path &root,
                            Origin origin, bool consented)
{
    ExtensionRecord record;
    record.id = manifest.id;
    record.name = manifest.name;
    record.version = manifest.version;
    record.origin = origin;
    record.consented = consented;
    record.root = root;
    record.dependencies = manifest.dependencies;
    record.menus = manifest.menus;
    return record;
}

// The contributions an enabled extension collects: its `root/packs` directory
// (when present) and the file URL of every contributed panel.
void collect_contributions(ExtensionRecord &record, const std::filesystem::path &root,
                           const NativeManifest &manifest)
{
    std::error_code ec;
    if (std::filesystem::is_directory(root / "packs", ec)) {
        record.pack_root = root / "packs";
    }
    for (const Panel &panel : manifest.panels) {
        PanelEntry entry;
        entry.label = panel.label;
        entry.qml = panel.qml;
        entry.url = file_url(root, panel.qml);
        record.panels.push_back(std::move(entry));
    }
}

// Depth-first search over `remaining` dependency edges for a path from `node`
// back to `start`. `path` accumulates the current traversal (starting at
// `start`); when an edge reaches `start`, the traversal is a cycle and the
// joined `start -> ... -> start` path is returned. `visited` keeps the search
// acyclic for nodes other than `start`.
std::optional<std::string>
search_cycle(const std::string &start, const std::string &node, std::vector<std::string> &path,
             std::set<std::string> &visited,
             const std::map<std::string, std::vector<std::string>> &requires_of,
             const std::set<std::string> &remaining)
{
    const auto it = requires_of.find(node);
    if (it == requires_of.end()) {
        return std::nullopt;
    }
    for (const std::string &dependency : it->second) {
        if (!remaining.count(dependency)) {
            continue;
        }
        if (dependency == start) {
            path.push_back(start);
            std::string joined;
            for (std::size_t i = 0; i < path.size(); ++i) {
                if (i != 0) {
                    joined += " -> ";
                }
                joined += path[i];
            }
            return joined;
        }
        if (visited.count(dependency)) {
            continue;
        }
        visited.insert(dependency);
        path.push_back(dependency);
        std::optional<std::string> found =
                search_cycle(start, dependency, path, visited, requires_of, remaining);
        if (found) {
            return found;
        }
        path.pop_back();
    }
    return std::nullopt;
}

// The cycle path `start -> ... -> start` when `start` can reach itself through
// `remaining` dependency edges, or empty when it cannot (it is a tail waiting
// on a cycle rather than a member of one).
std::string cycle_path(const std::string &start,
                       const std::map<std::string, std::vector<std::string>> &requires_of,
                       const std::set<std::string> &remaining)
{
    std::vector<std::string> path{ start };
    std::set<std::string> visited{ start };
    return search_cycle(start, start, path, visited, requires_of, remaining)
            .value_or(std::string{ });
}

// The first `remaining` dependency of `id`, or null when it has none (only a
// cycle member's own self-loop or a malformed graph reaches this).
const std::string *
first_remaining_dependency(const std::string &id,
                           const std::map<std::string, std::vector<std::string>> &requires_of,
                           const std::set<std::string> &remaining)
{
    const auto it = requires_of.find(id);
    if (it == requires_of.end()) {
        return nullptr;
    }
    for (const std::string &dependency : it->second) {
        if (remaining.count(dependency)) {
            return &dependency;
        }
    }
    return nullptr;
}

} // namespace

ExtensionHost::ExtensionHost(std::filesystem::path store_root, NativeExtension::Host host)
    : store_root_(std::move(store_root)), host_(std::move(host))
{
}

void ExtensionHost::scan()
{
    loaded_.clear();
    disabled_.clear();
    failed_.clear();
    natives_.clear();
    refusals_.clear();

    const Consent consent(store_root_);
    const DisabledList disabled_list(store_root_);
    origins_ = read_origins(store_root_);

    // Phase (a): enumerate, parse and validate every installed manifest. The
    // highest version per id is chosen; a Pack (an effect.toml) is skipped. A
    // manifest that fails to parse or validate is a failed record here, before
    // any dependency is resolved.
    struct Candidate
    {
        NativeManifest manifest;
        std::filesystem::path root;
        Origin origin = Origin::Developer;
        bool consented = false;
    };
    std::map<std::string, Candidate> candidates;

    for (const auto &[dir_id, versions] : installed_packs(store_root_)) {
        const std::filesystem::path *newest = nullptr;
        std::uint32_t newest_version = 0;
        for (const auto &[version, root] : versions) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(root / "extension.toml", ec)) {
                continue;
            }
            const std::uint32_t parsed = parse_version(version);
            if (newest == nullptr || parsed > newest_version) {
                newest = &root;
                newest_version = parsed;
            }
        }
        if (newest == nullptr) {
            continue; // no native extension under this id
        }

        const std::filesystem::path root = *newest;
        const NativeLoadResult manifest_result = load_native_manifest_file(root / "extension.toml");
        if (!manifest_result.manifest) {
            const std::string reason = manifest_result.problems.empty()
                    ? std::string("the manifest is unusable")
                    : manifest_result.problems.front().what;
            refusals_.push_back(dir_id + ": " + reason);
            ExtensionRecord record;
            record.id = dir_id; // the directory id: no usable manifest id
            record.state = ExtensionState::Failed;
            record.reason = reason;
            record.root = root;
            failed_.push_back(std::move(record));
            continue;
        }

        Candidate candidate;
        candidate.manifest = *manifest_result.manifest;
        candidate.root = root;
        candidate.origin = origin_of(candidate.manifest.id);
        candidate.consented = consent.consented(candidate.manifest.id);
        candidates[candidate.manifest.id] = std::move(candidate);
    }

    // Phase (b): build the dependency graph over the candidate ids and
    // Kahn-sort it. `requires_of[id]` lists the candidate ids `id` depends on;
    // `dependents[dep]` lists the ids that depend on `dep`; `indegree[id]`
    // counts `id`'s unresolved dependencies. A requirement naming a non-
    // candidate id is a missing dependency, resolved during the load phase.
    std::map<std::string, std::vector<std::string>> requires_of;
    std::map<std::string, std::vector<std::string>> dependents;
    std::map<std::string, std::size_t> indegree;
    for (const auto &[id, candidate] : candidates) {
        indegree[id] = 0;
    }
    for (const auto &[id, candidate] : candidates) {
        for (const std::string &required : candidate.manifest.dependencies) {
            if (candidates.count(required)) {
                requires_of[id].push_back(required);
                dependents[required].push_back(id);
                ++indegree[id];
            }
        }
    }

    std::set<std::string> ready;
    for (const auto &[id, degree] : indegree) {
        if (degree == 0) {
            ready.insert(id);
        }
    }
    std::vector<std::string> order;
    order.reserve(candidates.size());
    while (!ready.empty()) {
        const std::string id = *ready.begin();
        ready.erase(ready.begin());
        order.push_back(id);
        for (const std::string &dependent : dependents[id]) {
            if (--indegree[dependent] == 0) {
                ready.insert(dependent);
            }
        }
    }

    // Whatever never reached indegree 0 is a dependency cycle (or a tail that
    // waits on one). Every cycle member fails with the cycle named; a tail
    // fails with `requires <id>` for its blocked dependency.
    std::set<std::string> remaining;
    for (const auto &[id, degree] : indegree) {
        if (degree > 0) {
            remaining.insert(id);
        }
    }
    std::map<std::string, ExtensionState> state; // candidate id -> resolved
    for (const std::string &id : remaining) {
        const std::string cycle = cycle_path(id, requires_of, remaining);
        const std::string reason = cycle.empty()
                ? "requires " + std::string(*first_remaining_dependency(id, requires_of, remaining))
                : "dependency cycle: " + cycle;
        const Candidate &candidate = candidates.at(id);
        ExtensionRecord record = base_record(candidate.manifest, candidate.root, candidate.origin,
                                             candidate.consented);
        record.state = ExtensionState::Failed;
        record.reason = reason;
        failed_.push_back(std::move(record));
        refusals_.push_back(id + ": " + reason);
        state[id] = ExtensionState::Failed;
    }

    // Phase (c): load in topological order. Disabled first, then missing/
    // disabled/failed dependencies block the dependent, then load (a library
    // for a code extension, a record-only load for a data-only one).
    for (const std::string &id : order) {
        const Candidate &candidate = candidates.at(id);

        // A Builtin/Curated id the user disabled: visible, not loaded, and not
        // a refusal (there was nothing to refuse). Its reason is the one the
        // disable recorded (a direct disable records none); when none is
        // recorded but one of its own dependencies is not enabled, the reason
        // names that dependency instead.
        if (candidate.origin != Origin::Developer && disabled_list.disabled(id)) {
            ExtensionRecord record = base_record(candidate.manifest, candidate.root,
                                                 candidate.origin, candidate.consented);
            record.state = ExtensionState::Disabled;
            record.reason = disabled_list.reason(id);
            if (record.reason.empty()) {
                for (const std::string &required : candidate.manifest.dependencies) {
                    const auto dep = state.find(required);
                    if (dep == state.end() || dep->second != ExtensionState::Enabled) {
                        record.reason = "requires " + required;
                        break;
                    }
                }
            }
            disabled_.push_back(std::move(record));
            state[id] = ExtensionState::Disabled;
            continue;
        }

        // A required id that is missing, disabled or failed blocks this one.
        const std::string *blocked = nullptr;
        for (const std::string &required : candidate.manifest.dependencies) {
            const auto it = state.find(required);
            if (it == state.end() || it->second != ExtensionState::Enabled) {
                blocked = &required;
                break;
            }
        }
        if (blocked != nullptr) {
            const std::string reason = "requires " + *blocked;
            ExtensionRecord record = base_record(candidate.manifest, candidate.root,
                                                 candidate.origin, candidate.consented);
            record.state = ExtensionState::Failed;
            record.reason = reason;
            failed_.push_back(std::move(record));
            refusals_.push_back(id + ": " + reason);
            state[id] = ExtensionState::Failed;
            continue;
        }

        // A data-only extension contributes packs/panels but no library, so
        // there is nothing to dlopen.
        if (!candidate.manifest.entry) {
            ExtensionRecord record = base_record(candidate.manifest, candidate.root,
                                                 candidate.origin, candidate.consented);
            record.state = ExtensionState::Enabled;
            collect_contributions(record, candidate.root, candidate.manifest);
            loaded_.push_back(std::move(record));
            natives_.push_back(std::nullopt);
            state[id] = ExtensionState::Enabled;
            continue;
        }

        // Load. Trust gating (Developer consent, User refusal) and the failure
        // log both live in NativeExtension::load; the host collects the result.
        auto extension = NativeExtension::load(candidate.root / *candidate.manifest.entry, id,
                                               candidate.manifest.api, candidate.origin,
                                               candidate.consented, host_for(host_, id));
        if (!extension) {
            const std::string reason = extension.error();
            ExtensionRecord record = base_record(candidate.manifest, candidate.root,
                                                 candidate.origin, candidate.consented);
            record.state = ExtensionState::Failed;
            record.reason = reason;
            failed_.push_back(std::move(record));
            refusals_.push_back(id + ": " + reason);
            state[id] = ExtensionState::Failed;
            continue;
        }

        ExtensionRecord record = base_record(candidate.manifest, candidate.root, candidate.origin,
                                             candidate.consented);
        record.state = ExtensionState::Enabled;
        collect_contributions(record, candidate.root, candidate.manifest);
        loaded_.push_back(std::move(record));
        natives_.push_back(std::move(*extension));
        state[id] = ExtensionState::Enabled;
    }

    // Attach each record's dependents (the installed ids that require it), now
    // that the scan has resolved every record. The reverse edges were built in
    // phase (b) over candidate ids, so a missing dependency contributes none.
    const auto attach_dependents = [&](ExtensionRecord &record) {
        const auto it = dependents.find(record.id);
        if (it != dependents.end()) {
            record.dependents = it->second;
        }
    };
    for (ExtensionRecord &record : loaded_) {
        attach_dependents(record);
    }
    for (ExtensionRecord &record : disabled_) {
        attach_dependents(record);
    }
    for (ExtensionRecord &record : failed_) {
        attach_dependents(record);
    }
}

Origin ExtensionHost::origin_of(const std::string &id) const
{
    const auto it = origins_.find(id);
    return it == origins_.end() ? Origin::Developer : it->second;
}

std::vector<std::filesystem::path> ExtensionHost::pack_roots() const
{
    std::vector<std::filesystem::path> roots;
    roots.reserve(loaded_.size());
    for (const ExtensionRecord &entry : loaded_) {
        if (!entry.pack_root.empty()) {
            roots.push_back(entry.pack_root);
        }
    }
    return roots;
}

std::expected<std::string, std::string> ExtensionHost::invoke(const std::string &id,
                                                              const std::string &action,
                                                              const std::string &args) const
{
    for (std::size_t i = 0; i < loaded_.size(); ++i) {
        if (loaded_[i].id != id) {
            continue;
        }
        if (!natives_[i]) {
            return std::unexpected("the extension '" + id
                                   + "' is data-only and contributes no library");
        }
        return natives_[i]->invoke(action, args);
    }
    return std::unexpected("no loaded extension named '" + id + "'");
}

void ExtensionHost::on_project_opened(const std::string &path) const
{
    for (const auto &extension : natives_) {
        if (extension) {
            extension->on_project_opened(path);
        }
    }
}

void ExtensionHost::shutdown() const
{
    for (const auto &extension : natives_) {
        if (extension) {
            extension->on_shutdown();
        }
    }
}

} // namespace genesis::extensions
