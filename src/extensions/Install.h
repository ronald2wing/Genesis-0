// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "extensions/Sources.h"
#include "extensions/Trust.h"

namespace genesis::extensions {

// Installs the Extension at `source` into `store_root`, under the id and
// version its manifest declares. `source` is the Extension's directory, the
// one that holds its `extension.toml` manifest — the unified manifest every
// extension uses, whether it carries native code, QML, panels, or effect
// packs. A directory holding no `extension.toml` is refused. Validates the
// manifest, applies the trust policy, and refuses an Extension that fails
// either — WITHOUT writing anything to the store. Returns the installed
// Extension's root (`store_root/<id>/<version>`), or the reasons it was
// refused.
//
// A native extension is installed "visible" (unsigned, local-only, load-gated
// by recorded consent — see Consent.h and may_load_native): Builtin, Curated
// and Developer install, User is refused. A data-only or pack extension is
// gated through the same trust-policy path.
//
// A successful install also records the id's origin in the store's
// `origins.json` (see Origins.h), so ExtensionHost::scan can apply the right
// load gate; a reinstall/upgrade under a different origin overwrites the
// record (latest wins). A successful install also records the
// install source in `sources.json` (see Sources.h), so a config profile can
// reproduce the install elsewhere; the source record is best-effort, unlike
// the origin record (whose write failure refuses the install).
//
// `install_source` names where `source` came from. When its type is `Dir` and
// its url is empty, the absolute path of `source` is recorded instead, so an
// install from a local directory records a reproducible path with no extra
// caller work. Callers that already know the source (the builtin seed, a
// profile import) pass it explicitly.
//
// Two versions of the same id install side by side under their own version
// directory (`store_root/<id>/<v1>` and `store_root/<id>/<v2>`); reinstalling
// an id+version that is already present overwrites it deterministically.
struct InstallResult
{
    std::optional<std::filesystem::path> installed;
    std::vector<std::string> problems;
    bool ok() const { return installed.has_value(); }
};
InstallResult install_extension(const std::filesystem::path &source,
                                const std::filesystem::path &store_root, Origin origin,
                                InstallSource install_source = { SourceType::Dir, "" });

// Enumerates what is installed: id -> version -> root. Staging directories are
// skipped, so a store mid-install or a store left after a refused install shows
// only complete Extensions (Packs and native extensions alike - both install
// under `store_root/<id>/<version>`). An absent or empty store yields an empty
// map.
std::map<std::string, std::map<std::string, std::filesystem::path>>
installed_packs(const std::filesystem::path &store_root);

} // namespace genesis::extensions
