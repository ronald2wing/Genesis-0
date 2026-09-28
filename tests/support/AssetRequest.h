// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// One in-flight GES asset request, completed synchronously on a GMainLoop: the
// sync ges_asset_request returns null for an uncached asset, so the async form
// plus a loop is how a clip asset actually loads. Shared by the effect suites
// that need a real asset to build an effect chain.

#pragma once

#include <cstdio>

#include <ges/ges.h>

struct AssetRequest
{
    GMainLoop *loop = nullptr;
    GESAsset *asset = nullptr;
    GError *error = nullptr;
    bool timed_out = false;
};

inline void on_asset_ready(GObject *, GAsyncResult *result, gpointer user_data)
{
    auto *request = static_cast<AssetRequest *>(user_data);
    request->asset = ges_asset_request_finish(result, &request->error);
    g_main_loop_quit(request->loop);
}

inline gboolean on_asset_timeout(gpointer user_data)
{
    auto *request = static_cast<AssetRequest *>(user_data);
    request->timed_out = true;
    g_main_loop_quit(request->loop);
    return G_SOURCE_REMOVE;
}

// Bounded wait: a missing media file or a GES failure would otherwise block
// here until ctest's default timeout kills the suite.
inline GESAsset *request_asset(const gchar *uri)
{
    AssetRequest request;
    request.loop = g_main_loop_new(nullptr, FALSE);
    ges_asset_request_async(GES_TYPE_URI_CLIP, uri, nullptr, on_asset_ready, &request);
    const guint timeout_id = g_timeout_add(30000, on_asset_timeout, &request);
    g_main_loop_run(request.loop);
    if (!request.timed_out) {
        g_source_remove(timeout_id);
    }
    g_main_loop_unref(request.loop);
    if (request.timed_out) {
        std::fprintf(stderr, "request_asset: timed out waiting for %s\n", uri);
        return nullptr;
    }
    if (request.error != nullptr) {
        g_error_free(request.error);
        return nullptr;
    }
    return request.asset;
}
