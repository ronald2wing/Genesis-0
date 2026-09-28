// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>

namespace genesis::api {
struct Services;
} // namespace genesis::api

namespace genesis::cli {

// Exit codes shared by the CLI shell and every verb. 0 is success; any
// non-zero value is a refused or unreadable input. Only 1 is used today, but
// callers must not assume that.
inline constexpr int EXIT_OK = 0;
inline constexpr int EXIT_FAIL = 1;

// Each verb reads its input from disk, writes its report to `out`, and
// returns EXIT_OK on success or EXIT_FAIL on a refused/unreadable input.
// They write to a stream rather than to std::cout directly, so a caller (and
// the test suite) can drive them without spawning a process.

// `inspect` opens a project document and prints a summary: version, settings,
// timelines (name, track count, clip count, exact duration as num/den), media
// count, fonts. With `json` it emits one JSON object instead of text.
int cmd_inspect(std::ostream &out, const std::string &path, bool json);

// `validate` opens a project document and reports whether it loaded and why
// not when it did not. Exits non-zero on an unreadable document.
int cmd_validate(std::ostream &out, const std::string &path);

// `flatten` runs flatten_timeline over one timeline and prints the resulting
// clip list. `timeline_id` names the timeline; the active one when absent.
int cmd_flatten(std::ostream &out, const std::string &path,
                const std::optional<std::string> &timeline_id);

// `render` exports the project headlessly: resolves `preset` (the first
// default when absent) to a spec, flattens the active timeline, builds the
// host graph, and runs the export to completion through the engine seam,
// writing `output`. Prints a concise progress/result line; exits non-zero on
// an unreadable document, an unknown preset, an invalid spec, or a failed
// export.
int cmd_render(std::ostream &out, const std::string &path, const std::string &output,
               const std::optional<std::string> &preset);

// `packs` loads a Pack manifest and prints id/version/parameters/passes, or
// every load problem when the manifest does not validate.
int cmd_packs(std::ostream &out, const std::string &path);

// `packs trial` vets a candidate Pack directory safely: it loads and validates
// the manifest (reporting every problem), then - unless `validate_only` -
// compiles the Pack's GLSL body headlessly on a fresh offscreen context. No
// shader is ever executed and nothing is installed or trusted. Returns EXIT_OK
// only when the manifest validates (and the shader compiles, where a shader
// applies and a headless context is available); a valid Pack is reported as
// "validated, not trusted" with the trust/install path spelled out.
int cmd_packs_trial(std::ostream &out, const std::string &dir, bool validate_only);

// `commands` lists the host's command verbs, sorted, with a one-line
// description each - a discoverability aid for the Extension SDK.
int cmd_commands(std::ostream &out);

// `clear-cache` sweeps a processed-media cache root headlessly. With
// `--cap <bytes>` it evicts the oldest entries until the total fits the cap;
// without it, it removes every entry (and its sidecar). The CLI has no Qt, so
// the app's default cache root is unguessable - `--root` is required. Prints
// the before/after byte totals and the eviction count.
int cmd_clear_cache(std::ostream &out, const std::string &root,
                    const std::optional<std::uint64_t> &cap);

// The installed-Pack store used when no `--store` is given: the
// GENESIS_EXTENSIONS_DIR override, else `$XDG_DATA_HOME/genesis/extensions`,
// else `$HOME/.local/share/genesis/extensions`, else a relative fallback.
std::string default_extensions_store();

// `extensions list` enumerates an installed-Pack store: one heading per id,
// one line per version with its root. An absent store lists as empty.
int cmd_extensions_list(std::ostream &out, const std::string &store);

// `extensions install` runs install_from_source for the `source` directory into
// `store` under the named origin ("builtin", "curated", "developer" or
// "user"). Prints the installed root, or every refusal reason.
int cmd_extensions_install(std::ostream &out, const std::string &source, const std::string &store,
                           const std::string &origin);

// `extensions install --git <url> [--ref <ref>] [--subdir <path>]` clones the
// repository (optionally at `ref`, optionally into `subdir`) and installs from
// it. `ref` and `subdir` are empty/absent for the default branch and the
// repository root. Requires the `git` binary on PATH.
int cmd_extensions_install_git(std::ostream &out, const std::string &url,
                               const std::optional<std::string> &ref,
                               const std::optional<std::string> &subdir, const std::string &store,
                               const std::string &origin);

// `extensions catalog <url>` fetches and lists a marketplace catalog: one line
// per entry (id, version, name, source). The catalog's `revoked` list is
// reconciled into `store` (persisted best-effort; a failed write is reported
// as a `note:` but does not fail the list), so a later scan forces any revoked
// extension off. `fetcher` is the URL -> body fetch; empty (the default) falls
// back to the `curl` binary, which must be on PATH for a remote url (a local
// `file://` url works without one).
int cmd_extensions_catalog(
        std::ostream &out, const std::string &url, const std::string &store,
        std::function<std::optional<std::string>(const std::string &)> fetcher = { });

// `extensions install --catalog <url> <id>` fetches a catalog and installs the
// entry named `id` through its recorded source, under the named origin.
int cmd_extensions_install_catalog(std::ostream &out, const std::string &url, const std::string &id,
                                   const std::string &store, const std::string &origin);

// `extensions trust` reports the trust policy: the named origin's verdict and
// its granted capabilities, or all three origins when `origin` is absent.
int cmd_extensions_trust(std::ostream &out, const std::optional<std::string> &origin);

// `extensions remove` tombstones `id` (records it in the store's removed.json,
// so the next seed never re-adds it) and deletes its installed copy. Builtins
// only today: removing a store-installed extension is deferred. Exits non-zero
// when the tombstone cannot be recorded.
int cmd_extensions_remove(std::ostream &out, const std::string &store, const std::string &id);

// `extensions restore` clears `id`'s tombstone and re-seeds the builtins, so a
// removed builtin reappears. Exits non-zero when the tombstone cannot be
// cleared.
int cmd_extensions_restore(std::ostream &out, const std::string &store, const std::string &id);

// `extensions enable` / `extensions disable` toggle an installed native
// extension by origin (Developer is consent-gated; Builtin/Curated use the
// disabled list). Disabling also disables the transitive dependents. Exits
// non-zero when the store write fails.
int cmd_extensions_enable(std::ostream &out, const std::string &store, const std::string &id);
int cmd_extensions_disable(std::ostream &out, const std::string &store, const std::string &id);

// `config export` writes the store's portable profile (settings plus installed
// extensions with origin/source provenance) to `file` as JSON. The headless
// CLI wires no settings seam, so the profile's `settings` object is empty.
// Exits non-zero when the file cannot be written.
int cmd_config_export(std::ostream &out, const std::string &file, const std::string &store);

// `config import` reads a profile from `file` and applies it to `store`:
// settings through the seam (none in the headless CLI), extensions per the
// never-elevate-trust rules. With `no_install` every extension is skipped and
// only the settings are applied. Exits non-zero on an unreadable/unreadable
// profile.
int cmd_config_import(std::ostream &out, const std::string &file, const std::string &store,
                      bool no_install);

// The API services the CLI serves over `api`, `serve` and `serve --grpc`: the
// default extension store through its Manager seam, the builtin effect
// catalogue, and the media probe (the CLI links the engine adapter for
// `render`, so both the probe and the catalogue are wired for real).
genesis::api::Services cli_services();

// `api` serves the host's JSON-RPC API over stdio: one JSON request per line
// on `in`, one JSON reply per line on `out`. The method table and the shapes
// are the api/ module's; the single Editor is the one open project. Wired for
// headless automation. Returns EXIT_OK when the stream ends.
int cmd_api(std::istream &in, std::ostream &out);

} // namespace genesis::cli
