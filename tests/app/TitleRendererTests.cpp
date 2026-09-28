// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The title rasteriser's suite: drives the rasterisation and the keyed cache
// headlessly. `QImage` needs no display, but Qt6 does need a QGuiApplication
// for the font database QFontMetricsF and text painting draw from; the
// `offscreen` platform plugin serves that headlessly - the standard Qt raster
// platform, not a faked display. Fixtures live under the real temp directory,
// never a hardcoded path.

#include <QGuiApplication>
#include <QImage>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include <unistd.h>

#include "app/render/TitleRenderer.h"
#include "project/model/Text.h"

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

using genesis::app::render_title;
using genesis::app::RenderedTitle;
using genesis::app::title_cache_path;
using genesis::project::TextAlign;
using genesis::project::TextStyle;

// A scratch directory unique to this run, under the real temp directory.
std::filesystem::path scratch(const char *name)
{
    static int counter = 0;
    const auto dir = std::filesystem::temp_directory_path()
            / (std::string("genesis-title-renderer-") + name + "-" + std::to_string(::getpid())
               + "-" + std::to_string(counter++));
    std::filesystem::create_directories(dir);
    return dir;
}

// Pixels whose alpha is not zero.
int nontransparent(const QImage &image)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) != 0) {
                ++count;
            }
        }
    }
    return count;
}

// Whether two images differ in any pixel.
bool images_differ(const QImage &a, const QImage &b)
{
    if (a.size() != b.size()) {
        return true;
    }
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (a.pixel(x, y) != b.pixel(x, y)) {
                return true;
            }
        }
    }
    return false;
}

constexpr std::uint32_t kWidth = 320;
constexpr std::uint32_t kHeight = 180;

// A plain title: a family present on a normal Linux box (Qt falls back if it
// is not, so the tests hold either way), white text, centred.
TextStyle style()
{
    TextStyle s;
    s.font_family = "DejaVu Sans";
    s.font_size = 0.1;
    s.font_weight = 700.0;
    s.color = "#ffffff";
    s.align = TextAlign::Center;
    return s;
}

void test_renders_png_and_is_not_blank()
{
    const auto dir = scratch("png");
    const TextStyle s = style();

    const RenderedTitle text = render_title(s, "Hello world", kWidth, kHeight, dir);
    const RenderedTitle empty = render_title(s, "", kWidth, kHeight, dir);

    check(text.image.width() == static_cast<int>(kWidth)
                  && text.image.height() == static_cast<int>(kHeight),
          "the image has the requested dimensions");
    check(nontransparent(text.image) > 0, "rendered text is not blank");
    check(nontransparent(empty.image) == 0, "the empty render is blank");
    check(images_differ(text.image, empty.image), "text changes pixels versus the empty render");
    check(!text.cached, "the first render is not a cache hit");

    const std::filesystem::path png = title_cache_path(dir, s, "Hello world", kWidth, kHeight);
    check(std::filesystem::exists(png), "a PNG was written to the cache");
    if (std::filesystem::exists(png)) {
        QImage on_disk;
        check(on_disk.load(QString::fromStdString(png.string()))
                      && on_disk.size() == text.image.size(),
              "the on-disk PNG has the requested dimensions");
    }
}

void test_cache_reuses_and_changes()
{
    const auto dir = scratch("cache");
    TextStyle s = style();

    const RenderedTitle first = render_title(s, "Title A", kWidth, kHeight, dir);
    check(!first.cached, "the first render misses the cache");

    const RenderedTitle again = render_title(s, "Title A", kWidth, kHeight, dir);
    check(again.cached, "the same title reuses the cache entry");

    // A changed colour must not reuse the stale image.
    TextStyle recoloured = s;
    recoloured.color = "#ff0000";
    const RenderedTitle red = render_title(recoloured, "Title A", kWidth, kHeight, dir);
    check(!red.cached, "a changed style misses the cache");
    check(images_differ(first.image, red.image), "a changed style paints a different image");
    check(title_cache_path(dir, s, "Title A", kWidth, kHeight)
                  != title_cache_path(dir, recoloured, "Title A", kWidth, kHeight),
          "a changed style maps to a different cache file");

    // A changed text likewise.
    const RenderedTitle other = render_title(s, "Title B", kWidth, kHeight, dir);
    check(!other.cached, "a changed text misses the cache");
    check(images_differ(first.image, other.image), "a changed text paints a different image");
}

void test_unknown_font_falls_back()
{
    const auto dir = scratch("fallback");
    TextStyle s = style();
    s.font_family = "Definitely Not A Real Font Family 42";
    const RenderedTitle result = render_title(s, "Fallback", kWidth, kHeight, dir);
    check(result.image.width() == static_cast<int>(kWidth)
                  && result.image.height() == static_cast<int>(kHeight),
          "the fallback render has the requested dimensions");
    check(nontransparent(result.image) > 0, "an unknown font family falls back and still renders");
}

void test_empty_string_renders_blank()
{
    const auto dir = scratch("empty");
    const TextStyle s = style();
    const RenderedTitle result = render_title(s, "", kWidth, kHeight, dir);
    check(nontransparent(result.image) == 0, "empty text renders fully transparent");
    check(result.words.empty(), "empty text has no word boxes");
}

void test_reveal_map_one_rect_per_word()
{
    const auto dir = scratch("reveal");
    const TextStyle s = style();
    const RenderedTitle result = render_title(s, "one two three", kWidth, kHeight, dir);

    check(result.words.size() == 3, "three words, three boxes");
    check(result.words[0].x < result.words[1].x && result.words[1].x < result.words[2].x,
          "word boxes are in read order, left to right");
    check(result.reveal.width == kWidth && result.reveal.height == kHeight,
          "the reveal map is the canvas size");

    // A multi-word map spans order 0 to 255, one band per word.
    bool has_zero = false;
    bool has_max = false;
    for (const std::uint8_t value : *result.reveal.gray) {
        if (value == 0) {
            has_zero = true;
        }
        if (value == 255) {
            has_max = true;
        }
    }
    check(has_zero && has_max, "the multi-word reveal spans 0 to 255");
}

} // namespace

int main(int argc, char **argv)
{
    // See the file header: Qt6 needs a QGuiApplication for the font database,
    // and `offscreen` is the headless raster platform it runs on.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    test_renders_png_and_is_not_blank();
    test_cache_reuses_and_changes();
    test_unknown_font_falls_back();
    test_empty_string_renders_blank();
    test_reveal_map_one_rect_per_word();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
