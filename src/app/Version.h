// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The running application's version and the dotted-version comparison the
// notify-only update check uses. There was no single version source before
// this (the host CLI hardcodes "0.1.0" in its --version flag, unrelated); this
// is the one the app surfaces in Settings > Updates and compares the feed
// manifest's version against.
//
// CMake owns the value: GENESIS_APP_VERSION is passed as a compile definition
// to the app target. The macro fallback below keeps this header self-contained
// (the controller's test target does not receive the definition) and must stay
// in sync with CMakeLists.txt when the version bumps.

#pragma once

#include <string_view>

#include "workspace/update/UpdateVersion.h"

#ifndef GENESIS_APP_VERSION
#  define GENESIS_APP_VERSION "0.1.0"
#endif

namespace genesis::app {

inline constexpr std::string_view kAppVersion = GENESIS_APP_VERSION;

// The saturating dotted-version comparison, now owned by the host
// (genesis::workspace::update::compare_versions) so the updater's monotonic
// anti-downgrade/replay gate and this notify-only surface share one
// implementation. Re-exported here under the app namespace so existing callers
// keep genesis::app::compare_versions.
using genesis::workspace::update::compare_versions;

} // namespace genesis::app
