// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <gst/gst.h>
#include <optional>
#include <string>
#include <system_error>

#include "adapters/engine/ges/media/Filmstrip.h"
#include "adapters/engine/ges/media/Probe.h"
#include "project/command/MediaCommands.h"
#include "project/model/Media.h"

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
using genesis::project::MediaKind;
using genesis::project::NewMedia;

// Non-throwing existence+size probe, so a check that the file was made can
// fail (and report) instead of aborting the suite when it was not.
bool non_empty(const std::filesystem::path &path)
{
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec && std::filesystem::file_size(path, ec) > 0
            && !ec;
}

// A one-second 1280x720 VP8/WebM picture. Chosen so every aspect-derived size
// is exact: a tile of 80 wide is 44 high (80 * 720 / 1280 = 45, rounded even)
// and a poster of 480 wide is 270 high (480 * 720 / 1280 = 270).
std::string source_fixture()
{
    const std::filesystem::path generated =
            std::filesystem::temp_directory_path() / "genesis-filmstrip-source.webm";
    std::filesystem::remove(generated);
    const std::string command = "gst-launch-1.0 -q videotestsrc num-buffers=30 ! "
                                "video/x-raw,width=1280,height=720,framerate=30/1 ! videoconvert ! "
                                "vp8enc ! webmmux ! filesink location=\""
            + generated.string() + "\" >/dev/null 2>&1";
    const bool ok = std::system(command.c_str()) == 0 && std::filesystem::exists(generated)
            && std::filesystem::file_size(generated) > 0;
    return ok ? generated.string() : std::string{ };
}

// The filmstrip is one JPEG whose tiles read back at the expected grid size.
void test_makes_a_filmstrip()
{
    const std::string source = source_fixture();
    check(!source.empty(), "a source fixture is available");
    if (source.empty()) {
        return;
    }

    const std::filesystem::path destination =
            std::filesystem::temp_directory_path() / "genesis-filmstrip-output.jpg";
    std::filesystem::remove(destination);

    ges_adapter::FilmstripRequest request;
    request.source = source;
    request.destination = destination;
    request.columns = 4;
    request.tile_width = 80;

    const std::optional<std::filesystem::path> made = ges_adapter::make_filmstrip(request);
    check(made.has_value(), "make_filmstrip reports success");
    check(made.has_value() && *made == destination, "make_filmstrip returns the destination path");
    check(std::filesystem::exists(destination), "the filmstrip file exists");
    check(non_empty(destination), "the filmstrip file is non-empty");

    // The strip is a still, so the probe reads it as an image with the grid
    // dimensions: columns*tile_width wide by the even tile height.
    const std::optional<NewMedia> probed = ges_adapter::probe(destination);
    check(probed.has_value(), "the filmstrip probes");
    if (probed.has_value()) {
        check(probed->kind == MediaKind::Image, "the filmstrip probes as an image");
        check(probed->width.has_value() && *probed->width == 320,
              "filmstrip width is 4 tiles of 80");
        check(probed->height.has_value() && *probed->height == 44,
              "filmstrip height is the even aspect height");
    }

    std::filesystem::remove(destination);
    std::filesystem::remove(source);
}

// The poster is a single still at the fixed 480-wide, aspect-derived height.
void test_makes_a_poster()
{
    const std::string source = source_fixture();
    check(!source.empty(), "a source fixture is available");
    if (source.empty()) {
        return;
    }

    const std::filesystem::path destination =
            std::filesystem::temp_directory_path() / "genesis-poster-output.jpg";
    std::filesystem::remove(destination);

    const std::optional<std::filesystem::path> made =
            ges_adapter::make_poster(source, destination, 0.5);
    check(made.has_value(), "make_poster reports success");
    check(made.has_value() && *made == destination, "make_poster returns the destination path");
    check(std::filesystem::exists(destination), "the poster file exists");
    check(non_empty(destination), "the poster file is non-empty");

    const std::optional<NewMedia> probed = ges_adapter::probe(destination);
    check(probed.has_value(), "the poster probes");
    if (probed.has_value()) {
        check(probed->kind == MediaKind::Image, "the poster probes as an image");
        check(probed->width.has_value() && *probed->width == 480, "poster width is the fixed 480");
        check(probed->height.has_value() && *probed->height == 270,
              "poster height preserves the 16:9 aspect");
    }

    std::filesystem::remove(destination);
    std::filesystem::remove(source);
}

void test_a_missing_source_fails_cleanly()
{
    const std::filesystem::path missing =
            std::filesystem::temp_directory_path() / "genesis-no-such.webm";
    std::filesystem::remove(missing);
    const std::filesystem::path destination =
            std::filesystem::temp_directory_path() / "genesis-never-created.jpg";
    std::filesystem::remove(destination);

    ges_adapter::FilmstripRequest request;
    request.source = missing;
    request.destination = destination;

    check(!ges_adapter::make_filmstrip(request).has_value(),
          "a missing source returns nullopt for the filmstrip");
    check(!ges_adapter::make_poster(missing, destination, 0.0).has_value(),
          "a missing source returns nullopt for the poster");
    check(!std::filesystem::exists(destination), "a missing source creates no destination");
}

} // namespace

int main()
{
    gst_init(nullptr, nullptr);

    test_makes_a_filmstrip();
    test_makes_a_poster();
    test_a_missing_source_fails_cleanly();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
