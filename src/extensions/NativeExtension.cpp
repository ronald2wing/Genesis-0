// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/NativeExtension.h"

#include <dlfcn.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

namespace genesis::extensions {

namespace {

// The call_json trampoline handed to the extension. `host->reserved` is the
// Host* the loader parked there; the injected std::function lives behind it.
// All of this runs on the loading thread, synchronously.
int call_json_trampoline(const GenesisHostV1 *host, const char *method, const char *params,
                         char *out, std::uint32_t out_capacity)
{
    const auto *box = static_cast<const NativeExtension::Host *>(host->reserved);
    if (box == nullptr || !box->call_json) {
        return -GENESIS_EXT_ERR_FAILED;
    }
    std::string reply;
    const int rc =
            box->call_json(method != nullptr ? method : "", params != nullptr ? params : "", reply);
    if (rc != 0) {
        return -GENESIS_EXT_ERR_FAILED;
    }
    const std::uint64_t needed = reply.size() + 1;
    if (needed > out_capacity) {
        // The required size, as a positive value; nothing is written. Clamp so
        // the convention cannot overflow int.
        return static_cast<int>(std::min<std::uint64_t>(needed, std::numeric_limits<int>::max()));
    }
    if (needed > 0) {
        std::memcpy(out, reply.data(), needed);
    }
    return GENESIS_EXT_OK;
}

// The log trampoline handed to the extension.
void log_trampoline(const GenesisHostV1 *host, const char *message)
{
    const auto *box = static_cast<const NativeExtension::Host *>(host->reserved);
    if (box != nullptr && box->log && message != nullptr) {
        box->log(message);
    }
}

} // namespace

std::expected<NativeExtension, std::string>
NativeExtension::load(const std::filesystem::path &library_path, const std::string &expected_id,
                      std::uint32_t expected_api, Origin origin, bool consented, Host host)
{
    NativeExtension ext;
    ext.host_ = std::make_shared<Host>(std::move(host));

    // A refusal is logged to the sink when one is set, and returned as the
    // reason. The host never throws and never crashes on a bad library.
    const auto fail = [&](std::string reason) {
        if (ext.host_->log) {
            ext.host_->log("refusing to load " + library_path.string() + ": " + reason);
        }
        return std::expected<NativeExtension, std::string>(std::unexpected(std::move(reason)));
    };

    // 1. Trust: User is refused, Developer needs recorded consent, Builtin and
    //    Curated load freely.
    if (!may_load_native(origin, consented)) {
        return fail(origin == Origin::User ? "the extension's origin is not trusted"
                                           : "the developer extension has no recorded consent");
    }

    // 2. The manifest's declared API must match this host's.
    if (expected_api != GENESIS_EXTENSION_API_VERSION) {
        return fail("the extension targets API " + std::to_string(expected_api) + ", this host is "
                    + std::to_string(GENESIS_EXTENSION_API_VERSION));
    }

    // 3. Load the library. RTLD_LOCAL keeps its symbols out of the host's
    //    global namespace.
    void *handle = ::dlopen(library_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char *error_text = ::dlerror(); // read and clear the error once
        return fail(std::string("cannot load the library: ")
                    + (error_text != nullptr ? error_text : ""));
    }

    // 4. Resolve the entry. This is the one place a handle may be closed:
    //    dlsym ran no extension code, so dropping the library here is safe.
    auto *entry = reinterpret_cast<GenesisExtensionEntry>(::dlsym(handle, GENESIS_EXTENSION_ENTRY));
    if (entry == nullptr) {
        const char *error_text = ::dlerror(); // read and clear the error once
        const std::string error = error_text != nullptr ? error_text : std::string{ };
        ::dlclose(handle);
        return fail(std::string("the library exports no ") + GENESIS_EXTENSION_ENTRY
                    + (error.empty() ? std::string{ } : ": " + error));
    }

    // 5. Call the entry with a host table. From here on the library is never
    //    unloaded: the entry may have installed global state.
    ext.handle_ = handle;
    ext.host_table_ = std::make_shared<GenesisHostV1>();
    GenesisHostV1 &host_table = *ext.host_table_;
    host_table.size = sizeof(host_table);
    host_table.abi_version = GENESIS_EXTENSION_API_VERSION;
    host_table.call_json = call_json_trampoline;
    host_table.log = log_trampoline;
    host_table.reserved = ext.host_.get();

    GenesisExtensionV1 descriptor{ };
    const int rc = entry(&host_table, &descriptor);
    if (rc != GENESIS_EXT_OK) {
        return fail("the extension declined to load (code " + std::to_string(rc) + ")");
    }

    // 6. Validate the descriptor and cross-check its id against the manifest.
    if (descriptor.size < sizeof(GenesisExtensionV1)) {
        return fail("the extension's descriptor is smaller than this host "
                    "expects");
    }
    if (descriptor.id == nullptr || descriptor.id[0] == '\0') {
        return fail("the extension reported no id");
    }
    if (descriptor.id != expected_id) {
        return fail("the extension's id `" + std::string(descriptor.id)
                    + "` does not match the manifest's `" + expected_id + "`");
    }

    ext.id_ = descriptor.id;
    ext.vtable_ = descriptor;
    return ext;
}

std::expected<std::string, std::string> NativeExtension::invoke(const std::string &action,
                                                                const std::string &args) const
{
    if (vtable_.invoke == nullptr) {
        return std::unexpected("the extension contributes no invoke action");
    }
    char *result = nullptr;
    const int rc = vtable_.invoke(action.c_str(), args.c_str(), &result);
    if (rc != GENESIS_EXT_OK) {
        std::free(result);
        return std::unexpected("invoke failed (code " + std::to_string(rc) + ")");
    }
    std::string out = result != nullptr ? result : std::string{ };
    std::free(result);
    return out;
}

void NativeExtension::on_project_opened(const std::string &path) const
{
    if (vtable_.on_project_opened != nullptr) {
        vtable_.on_project_opened(path.c_str());
    }
}

void NativeExtension::on_shutdown() const
{
    if (vtable_.on_shutdown != nullptr) {
        vtable_.on_shutdown();
    }
}

} // namespace genesis::extensions
