// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "core/AtomicWrite.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace genesis::core {

bool write_file_atomic(const std::filesystem::path &path, std::string_view bytes)
{
    static std::atomic<std::uint64_t> counter{ 0 };

    // Create the staging file exclusively. A predictable name in a shared
    // directory is a symlink an attacker can pre-place, and a plain ofstream
    // would follow it; O_EXCL refuses instead, and a collision just tries the
    // next name.
    int descriptor = -1;
    std::filesystem::path staging;
    for (int attempt = 0; attempt < 8 && descriptor < 0; ++attempt) {
        staging = path.string() + ".tmp-" + std::to_string(::getpid()) + "-"
                + std::to_string(++counter);
        descriptor = ::open(staging.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (descriptor < 0 && errno != EEXIST) {
            return false;
        }
    }
    if (descriptor < 0) {
        return false;
    }

    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t chunk = ::write(descriptor, bytes.data() + written, bytes.size() - written);
        if (chunk <= 0) {
            ::close(descriptor);
            std::error_code ignored;
            std::filesystem::remove(staging, ignored);
            return false;
        }
        written += static_cast<std::size_t>(chunk);
    }
    // Flush before the rename: a crash must not leave the new name pointing at
    // bytes the kernel never wrote.
    if (::fsync(descriptor) != 0) {
        ::close(descriptor);
        std::error_code ignored;
        std::filesystem::remove(staging, ignored);
        return false;
    }
    ::close(descriptor);

    // rename(2) replaces an existing target atomically (and <filesystem>
    // specifies replace semantics portably). Removing the destination first
    // would leave the user with no file at all if the rename then failed.
    std::error_code ec;
    std::filesystem::rename(staging, path, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(staging, ignored);
        return false;
    }
    return true;
}

} // namespace genesis::core
