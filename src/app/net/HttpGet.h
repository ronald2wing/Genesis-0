// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// One bounded, synchronous GET for a small document: the shared helper the
// marketplace-catalog fetch and the update-feed fetch both reduce to. It runs
// on the calling thread over QNetworkAccessManager, follows safe redirects,
// and returns the body - or nullopt on any transport error, a redirect
// refusal, or the timeout lapsing. The header is deliberately Qt-free (only
// the .cpp sees QNetworkAccessManager), so the app's GES-before-Qt include
// ordering is untouched by including it.

#pragma once

#include <optional>
#include <string>

namespace genesis::app {

// Fetches `url` and returns its body, or nullopt on a transport error, a
// redirect refusal, or `timeout_ms` lapsing without the transfer completing.
// The default (10 s) suits small JSON documents; a caller streaming a large
// artifact uses its own roomier budget.
std::optional<std::string> http_get(const std::string &url, int timeout_ms = 10 * 1000);

} // namespace genesis::app
