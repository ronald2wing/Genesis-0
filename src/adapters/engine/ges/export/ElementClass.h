// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Factory classification shared by the decode policy and the hardware probe:
// GStreamer marks a hardware codec with a "/Hardware" suffix in its element
// klass, and that is the single source of truth for "can this machine decode in
// hardware".

#pragma once

#include <gst/gst.h>

namespace genesis::adapters::engine::ges {

inline bool is_hardware_decoder(GstElementFactory *factory)
{
    const gchar *klass = gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS);
    return klass != nullptr && g_str_has_suffix(klass, "Hardware");
}

} // namespace genesis::adapters::engine::ges
