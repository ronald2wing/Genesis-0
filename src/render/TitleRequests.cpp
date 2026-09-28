// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "render/TitleRequests.h"

#include <algorithm>

namespace genesis::render {

std::vector<TitleRequest> title_requests(const genesis::project::Project &project)
{
    // The active timeline is the one the engine builds and the app shows, so
    // its canvas is the one a title is rasterised against.
    const genesis::project::Timeline &timeline = project.active();

    // A title clip has no media; its words and look live in `Clip::text`. A
    // clip without a style, or with nothing to draw, asks for no picture.
    std::vector<const genesis::project::Clip *> titles;
    for (const genesis::project::Clip &clip : timeline.clips) {
        if (clip.kind != genesis::project::ClipKind::Text || !clip.text
            || clip.text->content.empty()) {
            continue;
        }
        titles.push_back(&clip);
    }

    // Clips are stored in insertion order, not time order; sort by `start`
    // so requests come back in timeline order. Stable, so two titles at the
    // same instant keep their insertion order.
    std::stable_sort(titles.begin(), titles.end(),
                     [](const genesis::project::Clip *a, const genesis::project::Clip *b) {
                         return a->start < b->start;
                     });

    std::vector<TitleRequest> out;
    out.reserve(titles.size());
    for (const genesis::project::Clip *clip : titles) {
        out.push_back(TitleRequest{ clip->id, clip->text->content, *clip->text,
                                    timeline.video.width, timeline.video.height });
    }
    return out;
}

} // namespace genesis::render
