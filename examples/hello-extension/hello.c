/*
 * SPDX-FileCopyrightText: 2026 Genesis-0 contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The hello-extension library: the smallest useful native Extension.
 *
 * It implements the one symbol the host resolves, `genesis_extension_entry`,
 * fills the GenesisExtensionV1 descriptor, and demonstrates both directions of
 * the C ABI:
 *   - the host calls the library (invoke("hello")), which returns JSON; and
 *   - the library calls the host (call_json("version", ...) and log), which is
 *     how an Extension reads the project and reports what it is doing.
 *
 * Build it with the CMakeLists.txt beside this file; see README.md.
 */

#include "extensions/ExtensionApi.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The host resolves the entry by name from the shared library. On Windows a
 * shared library only exports symbols marked dllexport; on ELF/Mach-O the
 * default visibility is enough, but the attribute is harmless. */
#if defined(_WIN32)
#define GENESIS_HELLO_EXPORT __declspec(dllexport)
#else
#define GENESIS_HELLO_EXPORT __attribute__((visibility("default")))
#endif

#define HELLO_ID "example.hello"
#define HELLO_ACTION "hello"

/* The host table received at entry. Retained so invoke and the notification
 * callbacks (which run after entry has returned) can still reach call_json and
 * log. The host guarantees callbacks run on the loading thread only, so this
 * needs no synchronization. */
static const GenesisHostV1* g_host = NULL;

/* Duplicates a NUL-terminated string with malloc. The host frees a returned
 * `*result` with free(), so it must not be a literal or a stack pointer. */
static char* dup_cstr(const char* text) {
    const size_t len = strlen(text);
    char* out = (char*)malloc(len + 1);
    if (out != NULL) {
        memcpy(out, text, len + 1);
    }
    return out;
}

static void log_line(const char* message) {
    if (g_host != NULL && g_host->log != NULL) {
        g_host->log(g_host, message);
    }
}

/* Routes one method to the host API bridge. Returns the ABI's own convention:
 * GENESIS_EXT_OK with a NUL-terminated reply in `out`, a positive required
 * size when `out` is too small, or a negative GENESIS_EXT_ERR_*. */
static int call_host(const char* method, const char* params, char* out,
                     uint32_t out_capacity) {
    if (g_host == NULL || g_host->call_json == NULL) {
        return -GENESIS_EXT_ERR_FAILED;
    }
    return g_host->call_json(g_host, method, params, out, out_capacity);
}

/* Runs one action the host named (a menu entry's `action`). */
static int invoke(const char* action, const char* args, char** result) {
    (void)args; /* this example's action takes no arguments */
    const char* name = action != NULL ? action : "";
    if (strcmp(name, HELLO_ACTION) != 0) {
        return -GENESIS_EXT_ERR_ARG;
    }

    /* Demonstrate the host bridge: ask the host for its version and log the
     * raw reply. The reply is a JSON envelope ({"result": ...}); this example
     * does not parse it. */
    char version_reply[512];
    const int rc = call_host("version", "{}", version_reply, sizeof(version_reply));
    if (rc != GENESIS_EXT_OK) {
        log_line("hello: call_json(version) failed");
        return -GENESIS_EXT_ERR_FAILED;
    }
    log_line("hello: invoke(hello) ran; host version reply follows");
    log_line(version_reply);

    /* Build the JSON this action returns. It must be malloc'd: the host takes
     * ownership and free()s it. */
    char buffer[256];
    const int written = snprintf(buffer, sizeof(buffer),
                                 "{\"message\":\"Hello from %s\",\"action\":\"%s\"}",
                                 HELLO_ID, HELLO_ACTION);
    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        return -GENESIS_EXT_ERR_FAILED;
    }
    *result = dup_cstr(buffer);
    return *result != NULL ? GENESIS_EXT_OK : -GENESIS_EXT_ERR_FAILED;
}

static void on_project_opened(const char* path) {
    (void)path;
    log_line("hello: project opened");
}

static void on_shutdown(void) {
    /* Called once, before the host tears down. No host callback may be used
     * after this returns. */
    log_line("hello: shutting down");
}

/* The one symbol the host resolves. Called once, right after dlopen. */
GENESIS_HELLO_EXPORT int genesis_extension_entry(const GenesisHostV1* host,
                                                 GenesisExtensionV1* ext) {
    if (host == NULL || ext == NULL || host->size < sizeof(GenesisHostV1) ||
        host->abi_version != GENESIS_EXTENSION_API_VERSION) {
        return -GENESIS_EXT_ERR_ABI;
    }
    g_host = host;

    ext->size = sizeof(*ext);
    ext->id = HELLO_ID;
    ext->invoke = invoke;
    ext->on_project_opened = on_project_opened;
    ext->on_shutdown = on_shutdown;
    return GENESIS_EXT_OK;
}
