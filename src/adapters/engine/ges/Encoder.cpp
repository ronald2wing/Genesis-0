// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "adapters/engine/ges/Encoder.h"

#include <gst/gst.h>

namespace genesis::adapters::engine::ges {

bool sink_accepts(const char *element, const char *format)
{
    GstElementFactory *factory = gst_element_factory_find(element);
    if (factory == nullptr) {
        return false;
    }
    const GList *templates = gst_element_factory_get_static_pad_templates(factory);
    bool accepted = false;
    for (const GList *node = templates; node != nullptr; node = node->next) {
        auto *templ = static_cast<GstStaticPadTemplate *>(node->data);
        if (templ->direction != GST_PAD_SINK) {
            continue;
        }
        GstCaps *template_caps = gst_static_caps_get(&templ->static_caps);
        GstCaps *wanted =
                gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, format, nullptr);
        accepted = gst_caps_can_intersect(template_caps, wanted);
        gst_caps_unref(wanted);
        gst_caps_unref(template_caps);
        if (accepted) {
            break;
        }
    }
    gst_object_unref(factory);
    return accepted;
}

} // namespace genesis::adapters::engine::ges
