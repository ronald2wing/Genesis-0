// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

#include <gst/gst.h>

#include "adapters/engine/ges/media/Probe.h"
#include "adapters/engine/ges/media/Proxy.h"
#include "project/command/MediaCommands.h"

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const std::string &what)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

namespace ges_adapter = genesis::adapters::engine::ges;
using genesis::project::NewMedia;

// A one-second 1280x720 VP8/WebM picture. Downscaled to a 360-high proxy its
// aspect-preserved width is exactly 640, so the size assertion is exact and
// not a tolerance around a fractional pixel count.
std::string source_fixture()
{
    const std::filesystem::path generated =
            std::filesystem::temp_directory_path() / "genesis-proxy-source.webm";
    std::filesystem::remove(generated);
    const std::string command = "gst-launch-1.0 -q videotestsrc num-buffers=30 ! "
                                "video/x-raw,width=1280,height=720,framerate=30/1 ! videoconvert ! "
                                "vp8enc ! webmmux ! filesink location=\""
            + generated.string() + "\" >/dev/null 2>&1";
    const bool ok = std::system(command.c_str()) == 0 && std::filesystem::exists(generated)
            && std::filesystem::file_size(generated) > 0;
    return ok ? generated.string() : std::string{ };
}

void test_makes_a_smaller_proxy()
{
    const std::string source = source_fixture();
    check(!source.empty(), "a source fixture is available");
    if (source.empty()) {
        return;
    }

    const std::filesystem::path destination =
            std::filesystem::temp_directory_path() / "genesis-proxy-output.webm";
    std::filesystem::remove(destination);

    ges_adapter::ProxyRequest request;
    request.source = source;
    request.destination = destination;
    request.height = 360;

    const std::optional<std::filesystem::path> made = ges_adapter::make_proxy(request);
    check(made.has_value(), "make_proxy reports success");
    check(made.has_value() && *made == destination, "make_proxy returns the destination path");
    check(std::filesystem::exists(destination), "the proxy file exists");
    check(std::filesystem::file_size(destination) > 0, "the proxy file is non-empty");

    const std::optional<NewMedia> probed = ges_adapter::probe(destination);
    check(probed.has_value(), "the proxy probes");
    if (probed.has_value()) {
        check(probed->width.has_value() && *probed->width == 640,
              "proxy width is 640 (aspect preserved from 1280x720 to 360)");
        check(probed->height.has_value() && *probed->height == 360,
              "proxy height is the requested 360");
    }

    std::filesystem::remove(destination);
    std::filesystem::remove(source);
}

void test_a_missing_source_fails_cleanly()
{
    const std::filesystem::path missing =
            std::filesystem::temp_directory_path() / "genesis-proxy-missing-source.webm";
    std::filesystem::remove(missing);
    const std::filesystem::path destination =
            std::filesystem::temp_directory_path() / "genesis-proxy-never-created.webm";
    std::filesystem::remove(destination);

    ges_adapter::ProxyRequest request;
    request.source = missing;
    request.destination = destination;
    request.height = 360;

    check(!ges_adapter::make_proxy(request).has_value(), "a missing source returns nullopt");
    check(!std::filesystem::exists(destination), "a missing source creates no destination");
}

} // namespace

int main()
{
    gst_init(nullptr, nullptr);

    test_makes_a_smaller_proxy();
    test_a_missing_source_fails_cleanly();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
