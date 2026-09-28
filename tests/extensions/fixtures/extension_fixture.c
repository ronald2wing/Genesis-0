/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Genesis-0 contributors
 *
 * A native-extension fixture built as a shared library (a MODULE target) for
 * the extension_host suite. It is compiled five times with different macros to
 * exercise the loader's refusal paths:
 *
 *   (no macro)              -> a well-behaved extension: reports id
 *                              "ext.fixture", implements invoke/on_project_
 *                              opened/on_shutdown, and routes a "version" call
 *                              through the host bridge inside invoke.
 *   FIXTURE_ABI_MISMATCH    -> the entry declines with GENESIS_EXT_ERR_ABI.
 *   FIXTURE_WRONG_ID        -> the entry reports an id that does not match the
 *                              manifest's, exercising the loader's cross-check.
 *   FIXTURE_NO_INVOKE       -> the entry reports no invoke (invoke is NULL),
 *                              so the loader's clean-refusal path is exercised.
 *   FIXTURE_NO_ENTRY        -> compiles to an empty library: no
 *                              genesis_extension_entry symbol at all.
 */

#include "extensions/ExtensionApi.h"

#ifndef FIXTURE_NO_ENTRY

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef FIXTURE_WRONG_ID
#define FIXTURE_ID "wrong.id"
#else
#define FIXTURE_ID "ext.fixture"
#endif

/* The callbacks and their helpers are unused when the entry declines (the ABI
 * variant) or when invoke is compiled out (the no-invoke variant), so they are
 * guarded to keep the build warning-free under -Werror. */
#ifndef FIXTURE_ABI_MISMATCH

/* The host table received at entry, retained so invoke (which runs after entry
 * has returned) can still reach the host's call_json. The loader parks its own
 * context in `reserved`, which the host-side trampolines read back. */
static const GenesisHostV1* g_host = NULL;

#ifndef FIXTURE_NO_INVOKE

/* Routes a call to the host bridge, or fails cleanly when it is absent. */
static int call_host(const char* method, const char* params, char* out,
                     uint32_t out_capacity) {
    if (g_host == NULL || g_host->call_json == NULL) {
        return -GENESIS_EXT_ERR_FAILED;
    }
    return g_host->call_json(g_host, method, params, out, out_capacity);
}

/* Echoes the action, prefixed before the host's "version" reply, proving both
 * the invoke path and the host bridge round-trip in one result string. */
static int invoke(const char* action, const char* args, char** result) {
    (void)args;
    char reply[256];
    const int rc = call_host("version", "{}", reply, sizeof(reply));
    if (rc != GENESIS_EXT_OK) {
        return rc;
    }
    const char* action_text = action != NULL ? action : "";
    const size_t action_len = strlen(action_text);
    const size_t reply_len = strlen(reply);
    char* out = (char*)malloc(action_len + 1 + reply_len + 1);
    if (out == NULL) {
        return -GENESIS_EXT_ERR_FAILED;
    }
    memcpy(out, action_text, action_len);
    out[action_len] = ':';
    memcpy(out + action_len + 1, reply, reply_len + 1);
    *result = out;
    return GENESIS_EXT_OK;
}
#endif /* FIXTURE_NO_INVOKE */

static void on_project_opened(const char* path) {
    (void)path;
    if (g_host != NULL && g_host->log != NULL) {
        g_host->log(g_host, "fixture: project opened");
    }
}

static void on_shutdown(void) {
    if (g_host != NULL && g_host->log != NULL) {
        g_host->log(g_host, "fixture: shutdown");
    }
}

#endif /* FIXTURE_ABI_MISMATCH */

int genesis_extension_entry(const GenesisHostV1* host, GenesisExtensionV1* ext) {
#ifdef FIXTURE_ABI_MISMATCH
    (void)host;
    (void)ext;
    return -GENESIS_EXT_ERR_ABI;
#else
    if (host == NULL || ext == NULL || host->size < sizeof(GenesisHostV1)) {
        return -GENESIS_EXT_ERR_ABI;
    }
    g_host = host;
    ext->size = sizeof(*ext);
    ext->id = FIXTURE_ID;
#ifndef FIXTURE_NO_INVOKE
    ext->invoke = invoke;
#endif
    ext->on_project_opened = on_project_opened;
    ext->on_shutdown = on_shutdown;
    return GENESIS_EXT_OK;
#endif
}

#endif /* FIXTURE_NO_ENTRY */
