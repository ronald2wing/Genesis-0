// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "core/UrlFetch.h"

#include <curl/curl.h>

#include <cstddef>
#include <fstream>
#include <system_error>

namespace genesis::core {

namespace {

// A catalog is a small JSON document; a hostile or misconfigured URL must not
// be able to make the CLI buffer an unbounded response.
constexpr std::size_t kMaxResponseBytes = 16 * 1024 * 1024;

struct MemSink
{
    std::string body;
    bool overflow = false;
};

struct FileSink
{
    std::ofstream *out;
    std::size_t max_bytes;
    std::size_t total = 0;
    bool overflow = false;
};

std::size_t write_body(char *data, std::size_t size, std::size_t count, void *user_data)
{
    auto *sink = static_cast<MemSink *>(user_data);
    const std::size_t bytes = size * count;
    if (bytes > kMaxResponseBytes || sink->body.size() > kMaxResponseBytes - bytes) {
        sink->overflow = true;
        return 0; // aborts the transfer with CURLE_WRITE_ERROR
    }
    sink->body.append(data, bytes);
    return bytes;
}

std::size_t write_file(char *data, std::size_t size, std::size_t count, void *user_data)
{
    auto *sink = static_cast<FileSink *>(user_data);
    const std::size_t bytes = size * count;
    if (bytes > sink->max_bytes || sink->total > sink->max_bytes - bytes) {
        sink->overflow = true;
        return 0;
    }
    sink->out->write(data, static_cast<std::streamsize>(bytes));
    if (!*sink->out) {
        return 0;
    }
    sink->total += bytes;
    return bytes;
}

// Shared options: TLS verification is on by default, a non-2xx is a failure,
// redirects are followed but only within the same protocol set, and no signal
// handler is installed.
CURL *make_curl(const std::string &url, char *error_buffer, const char *protocols, long timeout)
{
    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        return nullptr;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "genesis");
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, protocols);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, protocols);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    return curl;
}

} // namespace

std::optional<std::string> fetch_url(const std::string &url, std::string &error)
{
    char error_buffer[CURL_ERROR_SIZE] = { };
    CURL *curl = make_curl(url, error_buffer, "http,https,file", 30L);
    if (curl == nullptr) {
        error = "curl_easy_init failed";
        return std::nullopt;
    }

    MemSink sink;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);

    const CURLcode result = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (result != CURLE_OK) {
        error = error_buffer[0] != '\0' ? error_buffer : curl_easy_strerror(result);
        return std::nullopt;
    }
    if (sink.overflow) {
        error = "the response exceeds 16 MiB";
        return std::nullopt;
    }
    return sink.body;
}

bool download_file(const std::string &url, const std::filesystem::path &path, std::size_t max_bytes,
                   std::string &error)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "cannot create " + path.string();
        return false;
    }

    char error_buffer[CURL_ERROR_SIZE] = { };
    // A 256 MiB archive over a slow link can legitimately take minutes; the
    // connect timeout is the one that should fail fast.
    CURL *curl = make_curl(url, error_buffer, "http,https,file", 300L);
    if (curl == nullptr) {
        out.close();
        error = "curl_easy_init failed";
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        return false;
    }

    FileSink sink{ &out, max_bytes };
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);

    const CURLcode result = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    out.close();

    if (result != CURLE_OK) {
        error = error_buffer[0] != '\0' ? error_buffer : curl_easy_strerror(result);
    } else if (sink.overflow) {
        error = "the download exceeds " + std::to_string(max_bytes) + " bytes";
    } else if (!out) {
        error = "cannot write " + path.string();
    } else {
        return true;
    }

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return false;
}

} // namespace genesis::core
