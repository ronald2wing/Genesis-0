// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#ifndef GENESIS_EXTENSIONS_EXTENSION_API_H
#define GENESIS_EXTENSIONS_EXTENSION_API_H

/*
 * The public C ABI between a Genesis-0 host and a native extension.
 *
 * This header is the one contract a native extension is written against. It is
 * pure C (C99): no C++ types, no exceptions, no RTTI. The host loads the
 * extension's shared library, resolves `genesis_extension_entry`, and calls it
 * with a host table; the extension fills a descriptor the host then calls.
 *
 * Contract:
 * - The version is GENESIS_EXTENSION_API_VERSION. Each struct's `size` field is
 *   append-only: a writer sets `size` to `sizeof` the struct it filled, and a
 *   reader reads a field only when `size` covers it (offsetof(field) +
 *   sizeof(field) <= size). New fields are appended, never inserted or
 *   reordered, so an older library and a newer host (and vice versa) negotiate
 *   which fields exist by comparing `size`.
 * - Every `const char*` is UTF-8 and valid only for the duration of the call
 *   that passed it; a string returned to the host (a `*result` out-param) is
 *   allocated by the extension with malloc() and freed by the host with free().
 * - No exception and no C++ object crosses the boundary. A function reports
 *   failure by returning a negative GENESIS_EXT_ERR_* code; a callback never
 *   throws back into the host.
 * - Every callback the host hands the extension (call_json, log) runs on the
 *   loading thread (the host's main thread) only, synchronously, and is never
 *   called after on_shutdown has returned. The extension must not call one
 *   from any other thread, and must not retain the `host` table past entry.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GENESIS_EXTENSION_API_VERSION 1
#define GENESIS_EXTENSION_ENTRY "genesis_extension_entry"

/* Error codes. A non-zero return from any function the extension implements is
 * one of these, negated only for call_json's buffer-size convention below. */
#define GENESIS_EXT_OK 0
#define GENESIS_EXT_ERR_ABI 1 /* size / abi_version the reader does not know */
#define GENESIS_EXT_ERR_ID 2 /* id does not match the manifest's id */
#define GENESIS_EXT_ERR_ARG 3 /* null or empty argument */
#define GENESIS_EXT_ERR_FAILED 4 /* the operation failed */

struct GenesisHostV1;

/*
 * Routes a JSON-RPC request to the host's API and writes the JSON reply to
 * `out`, NUL-terminated, up to `out_capacity` bytes including the NUL. `host`
 * is the table the extension received at entry; its `reserved` field is the
 * host's own context, passed back uninterpreted.
 *
 * Returns GENESIS_EXT_OK on success, a negative GENESIS_EXT_ERR_* on failure,
 * or the required buffer size (a positive value) when `out` is too small, in
 * which case nothing is written.
 */
typedef int (*GenesisCallJson)(const struct GenesisHostV1 *host, const char *method,
                               const char *params, char *out, uint32_t out_capacity);

/* Sends a line to the host's log. `message` is valid for the call only. */
typedef void (*GenesisLog)(const struct GenesisHostV1 *host, const char *message);

/* The host table passed to `genesis_extension_entry`. */
typedef struct GenesisHostV1
{
    uint32_t size; /* sizeof(GenesisHostV1) */
    uint32_t abi_version; /* GENESIS_EXTENSION_API_VERSION */
    GenesisCallJson call_json;
    GenesisLog log;
    void *reserved; /* host-owned context, passed back to the callbacks */
} GenesisHostV1;

/*
 * Runs one action the host names (a menu entry's `action`). `args` is a JSON
 * object; `*result` (out) receives the JSON reply, malloc'd by the extension
 * and freed by the host with free(), or NULL for no payload. Returns
 * GENESIS_EXT_OK on success or a negative GENESIS_EXT_ERR_*.
 */
typedef int (*GenesisInvoke)(const char *action, const char *args, char **result);

/* Notified after a project is opened; `path` is valid for the call only. */
typedef void (*GenesisOnProjectOpened)(const char *path);

/* Notified before the host tears down. No host callback may be called after
 * this returns. */
typedef void (*GenesisOnShutdown)(void);

/* The descriptor the extension fills for the host at entry. */
typedef struct GenesisExtensionV1
{
    uint32_t size; /* sizeof(GenesisExtensionV1) */
    const char *id; /* namespaced id; must match the manifest's id */
    GenesisInvoke invoke; /* may be NULL: the extension contributes no actions */
    GenesisOnProjectOpened on_project_opened; /* may be NULL */
    GenesisOnShutdown on_shutdown; /* may be NULL */
} GenesisExtensionV1;

/*
 * The library's entry point. The host calls it once, right after loading the
 * library, with the host table `host` and an uninitialized `ext` the extension
 * fills. Returns GENESIS_EXT_OK on success, GENESIS_EXT_ERR_ABI when it does
 * not recognize `host`'s size/abi_version, or another negative code.
 */
typedef int (*GenesisExtensionEntry)(const GenesisHostV1 *host, GenesisExtensionV1 *ext);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GENESIS_EXTENSIONS_EXTENSION_API_H */
