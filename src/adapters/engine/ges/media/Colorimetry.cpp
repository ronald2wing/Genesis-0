// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "adapters/engine/ges/media/Colorimetry.h"

#include <gst/gst.h>
#include <gst/video/video-color.h>
#include <gst/video/video-info.h>

namespace genesis::adapters::engine::ges {

ScopeSignal signal_from_transfer(GstVideoTransferFunction transfer)
{
    switch (transfer) {
    case GST_VIDEO_TRANSFER_SMPTE2084:
    case GST_VIDEO_TRANSFER_BT2020_10:
    case GST_VIDEO_TRANSFER_BT2020_12:
        return ScopeSignal::Pq;
    case GST_VIDEO_TRANSFER_ARIB_STD_B67:
        return ScopeSignal::Hlg;
    default:
        return ScopeSignal::Sdr;
    }
}

ScopeSignal signal_from_caps(GstCaps *caps)
{
    if (caps == nullptr) {
        return ScopeSignal::Sdr;
    }

    // Raw video caps: gst_video_info_from_caps resolves the colorimetry (from
    // the `colorimetry` field or its default) and exposes the pixel format for
    // the 10-bit hint.
    GstVideoInfo info;
    gst_video_info_init(&info);
    if (gst_video_info_from_caps(&info, caps)) {
        const ScopeSignal signal = signal_from_transfer(info.colorimetry.transfer);
        if (signal != ScopeSignal::Sdr) {
            return signal;
        }
        // No tagged HDR transfer. An explicitly SDR transfer (BT.709, sRGB, a
        // gamma curve) stays SDR; only an untagged 10-bit picture is the HDR
        // hint (HDR10 is PQ), so a 10-bit SDR master tagged with its transfer
        // is not misread.
        if (info.colorimetry.transfer == GST_VIDEO_TRANSFER_UNKNOWN && info.finfo != nullptr
            && GST_VIDEO_FORMAT_INFO_DEPTH(info.finfo, 0) >= 10) {
            return ScopeSignal::Pq;
        }
        return ScopeSignal::Sdr;
    }

    // Encoded caps (the discoverer reports codec caps, not raw video): read
    // the tagged colorimetry string the container/codec carried.
    const GstStructure *structure = gst_caps_get_structure(caps, 0);
    if (structure == nullptr) {
        return ScopeSignal::Sdr;
    }
    const gchar *colorimetry = gst_structure_get_string(structure, "colorimetry");
    if (colorimetry == nullptr) {
        return ScopeSignal::Sdr;
    }
    GstVideoColorimetry parsed;
    if (!gst_video_colorimetry_from_string(&parsed, colorimetry)) {
        return ScopeSignal::Sdr;
    }
    return signal_from_transfer(parsed.transfer);
}

} // namespace genesis::adapters::engine::ges
