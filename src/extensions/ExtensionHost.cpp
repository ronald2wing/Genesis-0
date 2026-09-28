// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/ExtensionHost.h"

#include <algorithm>
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
#include "extensions/RevokedList.h"
#include "extensions/Trust.h"

namespace genesis::extensions {

namespace {

// The refusal reason (and log line) when an untrusted origin declares an
// `overrides` entry. Shared verbatim by the install gate (Install.cpp) so a
// human — or a test — can match it across both paths.
constexpr const char *kOverridesRefused =
        "overrides are refused: only Builtin, Curated and Developer extensions may override";

// The trust rank an origin contributes to override resolution: a higher rank
// wins a collision, so Builtin beats Curated beats Developer beats User.
int trust_rank(Origin origin)
{
    switch (origin) {
    case Origin::Builtin:
        return 3;
    case Origin::Curated:
        return 2;
    case Origin::Developer:
        return 1;
    case Origin::User:
        return 0;
    }
    return 0;
}

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
ExtensionRecord base_record(const ExtensionManifest &manifest, const std::filesystem::path &root,
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
    record.overrides = manifest.overrides;
    return record;
}

// The contributions an enabled extension collects: its `root/packs` directory
// (when present) and the file URL of every contributed panel.
void collect_contributions(ExtensionRecord &record, const std::filesystem::path &root,
                           const ExtensionManifest &manifest)
{
    record.effects = manifest.effects;
    std::error_code ec;
    if (std::filesystem::is_directory(root / "packs", ec)) {
        record.pack_root = root / "packs";
    }
    for (const Panel &panel : manifest.panels) {
        PanelEntry entry;
        entry.id = panel.id;
        entry.label = panel.label;
        entry.qml = panel.qml;
        entry.url = file_url(root, panel.qml);
        record.panels.push_back(std::move(entry));
    }
    for (const Dialog &dialog : manifest.dialogs) {
        DialogEntry entry;
        entry.id = dialog.id;
        entry.label = dialog.label;
        entry.qml = dialog.qml;
        entry.url = file_url(root, dialog.qml);
        entry.menu_path = dialog.menu_path;
        record.dialogs.push_back(std::move(entry));
    }
    for (const Slot &slot : manifest.slot_entries) {
        SlotEntry entry;
        entry.id = record.id;
        entry.point = slot.point;
        entry.url = file_url(root, slot.qml);
        entry.priority = slot.priority;
        record.slot_entries.push_back(std::move(entry));
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

// Applies override resolution to one kind of contribution (menus, panels, or
// dialogs) in place. Two rules decide what survives, both keyed on the
// capability id:
//
// - Ownership: a capability id `X` is owned by the candidate extension `E` when
//   `X == E` or `X` starts with `E + ":"`. An extension contributes its own and
//   free (unowned) ids freely; it contributes another's owned id only when it
//   declares a matching `<kind>:<X>` override, otherwise the contribution is
//   skipped and logged.
// - Collision: among the contributions that remain for one id, the highest trust
//   rank wins (Builtin > Curated > Developer > User), ties broken by the
//   lexicographically smallest extension id. Losers are skipped and logged.
//
// An override entry that matches nothing (its extension does not contribute
// that (kind, id), or the id is not owned by another candidate) is logged as a
// warning, not an error.
template <typename Entry>
void resolve_contributions(std::vector<ExtensionRecord> &loaded,
                           const std::set<std::string> &candidate_ids,
                           const std::map<std::string, std::vector<OverrideEntry>> &overrides_of,
                           CapabilityKind kind, const char *kind_name,
                           std::vector<Entry> ExtensionRecord::*member,
                           const NativeExtension::Host &host)
{
    const auto log = [&host](const std::string &message) {
        if (host.log) {
            host.log(message);
        }
    };

    // The owner of a capability id X: the candidate E with X == E or X starting
    // with E + ":". Candidate ids are namespaced, so this owner is unique when
    // it exists.
    const auto owner_of = [&candidate_ids](std::string_view id) -> const std::string * {
        for (const std::string &candidate : candidate_ids) {
            if (id == candidate || id.starts_with(candidate + ":")) {
                return &candidate;
            }
        }
        return nullptr;
    };

    // The other extension owning `id`, or null when `id` is `owner_id`'s own or
    // is free (unowned).
    const auto owned_by_other = [&owner_of](std::string_view id,
                                            const std::string &owner_id) -> const std::string * {
        const std::string *owner = owner_of(id);
        return (owner != nullptr && *owner != owner_id) ? owner : nullptr;
    };

    // Whether `overrides` names a `<kind>:<id>` entry.
    const auto has_override = [&](const std::vector<OverrideEntry> &overrides,
                                  std::string_view id) {
        for (const OverrideEntry &entry : overrides) {
            if (entry.kind == kind && entry.id == id) {
                return true;
            }
        }
        return false;
    };

    // Unused-override warnings, judged against each extension's declared (still
    // unfiltered) contributions: an override that names a (kind, id) its
    // extension does not contribute, or an id no other candidate owns, matches
    // nothing.
    for (const ExtensionRecord &record : loaded) {
        const auto overrides_it = overrides_of.find(record.id);
        if (overrides_it == overrides_of.end()) {
            continue;
        }
        for (const OverrideEntry &entry : overrides_it->second) {
            if (entry.kind != kind) {
                continue;
            }
            bool contributes = false;
            for (const Entry &contribution : record.*member) {
                if (contribution.id == entry.id) {
                    contributes = true;
                    break;
                }
            }
            if (!contributes || owned_by_other(entry.id, record.id) == nullptr) {
                log("extension " + record.id + " override " + kind_name + ":" + entry.id
                    + " matches nothing");
            }
        }
    }

    // Ownership filter: group each surviving contribution under its capability
    // id as (record index, entry index) into `loaded`; skip and log one that
    // claims another extension's owned id without an override.
    std::map<std::string, std::vector<std::pair<std::size_t, std::size_t>>> groups;
    std::set<std::pair<std::size_t, std::size_t>> dropped;
    for (std::size_t ri = 0; ri < loaded.size(); ++ri) {
        ExtensionRecord &record = loaded[ri];
        const auto overrides_it = overrides_of.find(record.id);
        const std::vector<Entry> &entries = record.*member;
        for (std::size_t ei = 0; ei < entries.size(); ++ei) {
            const std::string &cap_id = entries[ei].id;
            const std::string *other = owned_by_other(cap_id, record.id);
            if (other != nullptr) {
                const bool overridden = overrides_it != overrides_of.end()
                        && has_override(overrides_it->second, cap_id);
                if (!overridden) {
                    log("extension " + record.id + " declares " + kind_name + " " + cap_id
                        + " owned by " + *other + " without an override; skipped");
                    dropped.emplace(ri, ei);
                    continue;
                }
            }
            groups[cap_id].emplace_back(ri, ei);
        }
    }

    // Collision resolution per id: an explicitly declared override wins over
    // trust rank (an override means the extension intends to replace the
    // contribution). When neither or both declare an override, the highest
    // trust rank wins, ties by the smallest extension id. Every loser is
    // skipped and logged.
    const auto has_override_entry = [&](std::size_t ri, std::string_view cap_id) {
        const auto ov = overrides_of.find(loaded[ri].id);
        return ov != overrides_of.end() && has_override(ov->second, cap_id);
    };
    for (auto &[cap_id, group] : groups) {
        std::size_t winner = 0;
        bool winner_overrides = has_override_entry(group[0].first, cap_id);
        for (std::size_t g = 1; g < group.size(); ++g) {
            const bool cur_overrides = has_override_entry(group[g].first, cap_id);
            // An explicit override wins over a non-override regardless of
            // trust rank: the override is the extension's declared intent to
            // replace the contribution.
            if (cur_overrides != winner_overrides) {
                if (cur_overrides) {
                    winner = g;
                    winner_overrides = true;
                }
                continue;
            }
            // Both override or both do not: trust rank decides.
            const ExtensionRecord &cur = loaded[group[g].first];
            const ExtensionRecord &win = loaded[group[winner].first];
            const int cur_rank = trust_rank(cur.origin);
            const int win_rank = trust_rank(win.origin);
            if (cur_rank > win_rank || (cur_rank == win_rank && cur.id < win.id)) {
                winner = g;
            }
        }
        for (std::size_t g = 0; g < group.size(); ++g) {
            if (g == winner) {
                continue;
            }
            log("extension " + loaded[group[g].first].id + " " + kind_name + " " + cap_id
                + " loses to " + loaded[group[winner].first].id);
            dropped.emplace(group[g].first, group[g].second);
        }
    }

    // Rebuild each record's vector without the dropped entries, preserving
    // manifest order. Done after all logging so no moved-from entry is read
    // again.
    for (std::size_t ri = 0; ri < loaded.size(); ++ri) {
        std::vector<Entry> &entries = loaded[ri].*member;
        std::vector<Entry> kept;
        kept.reserve(entries.size());
        for (std::size_t ei = 0; ei < entries.size(); ++ei) {
            if (dropped.count({ ri, ei }) == 0) {
                kept.push_back(std::move(entries[ei]));
            }
        }
        entries = std::move(kept);
    }
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
    revoked_.clear();
    failed_.clear();
    natives_.clear();
    refusals_.clear();

    const Consent consent(store_root_);
    const DisabledList disabled_list(store_root_);
    origins_ = read_origins(store_root_);

    // Phase (a): enumerate, parse and validate every installed manifest. The
    // highest version per id is chosen; a data-only extension (no `entry`) is
    // skipped for code contributions. A manifest that fails to parse or
    // validate is a failed record here, before any dependency is resolved.
    struct Candidate
    {
        ExtensionManifest manifest;
        std::filesystem::path root;
        Origin origin = Origin::Developer;
        bool consented = false;
        std::string version; // the version directory name, for the revocation match
    };
    std::map<std::string, Candidate> candidates;

    for (const auto &[dir_id, versions] : installed_packs(store_root_)) {
        const std::filesystem::path *newest = nullptr;
        std::uint32_t newest_version = 0;
        std::string newest_version_string;
        for (const auto &[version, root] : versions) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(root / "extension.toml", ec)) {
                continue;
            }
            const std::uint32_t parsed = parse_version(version);
            if (newest == nullptr || parsed > newest_version) {
                newest = &root;
                newest_version = parsed;
                newest_version_string = version;
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
        candidate.version = newest_version_string;

        // Trust gate for overrides: Builtin, Curated and Developer may declare
        // them; User is refused. A developer override is a declarative/QML/asset
        // swap, not arbitrary host-code execution — consent is the gate, not
        // origin. The install-time gate mirrors this check.
        // A User extension that declares an `overrides` entry is refused here
        // (before it joins the dependency graph), exactly as the install-time
        // gate refuses it before writing to the store.
        if (!candidate.manifest.overrides.empty() && !may_override(candidate.origin)) {
            refusals_.push_back(candidate.manifest.id + ": " + kOverridesRefused);
            if (host_.log) {
                host_.log(kOverridesRefused);
            }
            ExtensionRecord record = base_record(candidate.manifest, candidate.root,
                                                 candidate.origin, candidate.consented);
            record.state = ExtensionState::Failed;
            record.reason = kOverridesRefused;
            failed_.push_back(std::move(record));
            continue;
        }

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

    // Phase (c): load in topological order. Revoked first (a forced-off that
    // overrides enabled/disabled), then disabled, then missing/disabled/failed
    // dependencies block the dependent, then load (a library for a code
    // extension, a record-only load for a data-only one).
    for (const std::string &id : order) {
        const Candidate &candidate = candidates.at(id);

        // An id the store's revocation list names at its loaded version: it
        // does not load regardless of origin or disable state, and it is not a
        // refusal (there was nothing to refuse). Its reason is the publisher's,
        // or the bare "revoked" when the catalog gave none. The files stay on
        // disk so the user can remove it.
        const std::optional<Revocation> revocation = revoked_at(store_root_, id, candidate.version);
        if (revocation) {
            ExtensionRecord record = base_record(candidate.manifest, candidate.root,
                                                 candidate.origin, candidate.consented);
            record.state = ExtensionState::Revoked;
            record.reason = revocation->reason.value_or("revoked");
            revoked_.push_back(std::move(record));
            state[id] = ExtensionState::Revoked;
            continue;
        }

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

    // Override resolution: filter the loaded extensions' menu/panel/dialog
    // contributions to their winners, applying the ownership and trust rules.
    // `candidate_ids` is every installed extension id (loaded or not); the
    // overrides map carries each candidate's declared `overrides` entries.
    {
        std::set<std::string> candidate_ids;
        std::map<std::string, std::vector<OverrideEntry>> overrides_of;
        for (const auto &[id, candidate] : candidates) {
            candidate_ids.insert(id);
            overrides_of[id] = candidate.manifest.overrides;
        }
        resolve_contributions(loaded_, candidate_ids, overrides_of, CapabilityKind::Action,
                              "action", &ExtensionRecord::menus, host_);
        resolve_contributions(loaded_, candidate_ids, overrides_of, CapabilityKind::Panel, "panel",
                              &ExtensionRecord::panels, host_);
        resolve_contributions(loaded_, candidate_ids, overrides_of, CapabilityKind::Dialog,
                              "dialog", &ExtensionRecord::dialogs, host_);
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
    for (ExtensionRecord &record : revoked_) {
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

std::vector<SlotEntry> ExtensionHost::slot_entries() const
{
    std::vector<SlotEntry> out;
    for (const ExtensionRecord &entry : loaded_) {
        out.insert(out.end(), entry.slot_entries.begin(), entry.slot_entries.end());
    }
    std::stable_sort(out.begin(), out.end(), [](const SlotEntry &a, const SlotEntry &b) {
        if (a.priority != b.priority) {
            return a.priority < b.priority;
        }
        return a.id < b.id;
    });
    return out;
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