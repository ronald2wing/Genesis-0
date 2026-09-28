// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "extensions/ExtensionApi.h"
#include "extensions/Trust.h"

namespace genesis::extensions {

// A native extension loaded into the process from a shared library. The loader
// is POSIX today (dlopen/dlsym on Linux); Windows and macOS are a follow-on
// that swaps only the handle/unload primitives, not this class's surface.
//
// A library is loaded once and never unloaded for v1: the extension may have
// installed global state (registrations, atexit handlers, threads) that a
// dlclose would tear out from under, so the handle is deliberately held for
// the process lifetime. Only the dlsym-miss path closes a handle, because the
// entry never ran there.
//
// Threading: every callback the extension reaches (call_json, log) runs on the
// loading thread only; this class owns no thread and does no synchronization,
// in the style of jobs::Scheduler - drive it from the host's main thread.
class NativeExtension
{
public:
    // The injected seams, in the style of api::Services: the host API bridge
    // the extension's call_json reaches, and the sink its log messages go to.
    // The app binds `call_json` to api::dispatch and `log` to its own logger.
    //
    // `call_json` returns 0 on success (with the JSON reply written to `out`,
    // which may itself be a JSON-RPC error envelope) and any non-zero value on
    // bridge failure; a non-zero becomes GENESIS_EXT_ERR_FAILED at the ABI.
    struct Host
    {
        std::function<int(const std::string &method, const std::string &params, std::string &out)>
                call_json;
        std::function<void(const std::string &message)> log;
    };

    // Loads the library at `library_path`, resolves the entry symbol, and
    // returns the loaded extension. The load is refused - and the reason logged
    // to `host.log` when one is set - before any extension code runs, unless
    // the library itself declines in its entry.
    //
    // `expected_id` and `expected_api` come from the extension's manifest: the
    // id the library reports in its descriptor must match, and the manifest's
    // declared API must match this host's GENESIS_EXTENSION_API_VERSION.
    // `origin` and `consented` gate trust (see may_load_native): User is
    // refused, Developer loads only with recorded consent, Builtin/Curated
    // load freely.
    static std::expected<NativeExtension, std::string>
    load(const std::filesystem::path &library_path, const std::string &expected_id,
         std::uint32_t expected_api, Origin origin, bool consented, Host host);

    // Move-only: owns the dlopen handle.
    NativeExtension(NativeExtension &&) noexcept = default;
    NativeExtension &operator=(NativeExtension &&) noexcept = default;
    ~NativeExtension() = default; // no dlclose: never unload a native extension
    NativeExtension(const NativeExtension &) = delete;
    NativeExtension &operator=(const NativeExtension &) = delete;

    // The id the library reported (equal to the manifest's id after the
    // cross-check in load).
    const std::string &id() const { return id_; }

    // Calls the extension's invoke(action, args). Returns the JSON result the
    // extension produced, or why it failed. Fails cleanly (no crash) when the
    // extension contributed no invoke.
    std::expected<std::string, std::string> invoke(const std::string &action,
                                                   const std::string &args) const;

    void on_project_opened(const std::string &path) const;
    void on_shutdown() const;

private:
    NativeExtension() = default;

    void *handle_ = nullptr;
    std::string id_;
    GenesisExtensionV1 vtable_{ };
    std::shared_ptr<Host> host_; // stable address, pointed to by `reserved`
    // The host table handed to the entry. Heap-allocated so its address stays
    // valid after load returns: the extension retains the pointer (the fixture
    // parks it in a static) and dereferences it from invoke, which runs later.
    std::shared_ptr<GenesisHostV1> host_table_;
};

} // namespace genesis::extensions
