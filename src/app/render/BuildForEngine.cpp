// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/render/BuildForEngine.h"

#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "app/render/TitleRenderer.h"
#include "render/Flatten.h"
#include "render/TitleRequests.h"

namespace render = genesis::render;

genesis::render::BuiltTimeline
build_for_engine(const genesis::project::Project &project, const std::filesystem::path &cache_root,
                 std::optional<std::filesystem::path> proxy_cache_root,
                 std::optional<std::filesystem::path> mask_root,
                 const genesis::render::Catalogue *catalogue)
{
    const std::vector<render::ExportClip> flat =
            render::flatten_timeline_in(project, std::nullopt, mask_root);

    // Titles rasterise separately and rejoin as plain image clips: paint each
    // one into the cache before the graph is built, collecting clip id -> PNG
    // path. A title that cannot be painted is simply absent from the map and
    // the app still runs. The per-word reveal map comes back from the same
    // render call (RenderedTitle::reveal), so it is collected alongside under
    // the same key and handed over with the pictures.
    std::unordered_map<std::string, std::string> title_images;
    std::unordered_map<std::string, genesis::core::RevealMap> reveal_maps;
    for (const render::TitleRequest &title : render::title_requests(project)) {
        const std::filesystem::path png = genesis::app::title_cache_path(
                cache_root, title.style, title.text, title.canvas_width, title.canvas_height);
        if (png.empty()) {
            continue;
        }
        const genesis::app::RenderedTitle rendered = genesis::app::render_title(
                title.style, title.text, title.canvas_width, title.canvas_height, cache_root);
        if (std::filesystem::exists(png)) {
            title_images[title.clip_id] = png.string();
            reveal_maps[title.clip_id] = rendered.reveal;
        }
    }

    render::ExportRequest request;
    request.width = 1280;
    request.height = 720;
    return render::build_timeline(request, genesis::core::FrameRate::THIRTY,
                                  std::span<const render::ExportClip>(flat), { },
                                  std::move(title_images), std::move(reveal_maps), cache_root,
                                  proxy_cache_root, catalogue);
}
