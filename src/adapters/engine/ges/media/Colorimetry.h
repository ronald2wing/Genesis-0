// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <gst/video/video-color.h>

#include "adapters/engine/ScopeTypes.h"

// GstCaps stays opaque here: the header does not pull in <gst/gst.h>.
typedef struct _GstCaps GstCaps;

namespace genesis::adapters::engine::ges {

// Maps a GStreamer video transfer function (the colorimetry's transfer
// component) to the scope signal it implies. SMPTE ST 2084 (PQ) and the
// BT.2020 10/12-bit PQ-family transfers read as Pq; ARIB STD-B67 (HLG) reads
// as Hlg; everything else (BT.709, sRGB, gamma, unknown) reads as Sdr.
ScopeSignal signal_from_transfer(GstVideoTransferFunction transfer);

// The scope signal a frame's caps imply. Raw video/x-raw caps are resolved
// through gst_video_info_from_caps: the tagged transfer characteristic when
// present, else a 10-bit picture as the HDR hint, else Sdr. Encoded caps (what
// the discoverer reports) fall back to their `colorimetry` string field. Pure
// caps parsing - no GL or pipeline state.
ScopeSignal signal_from_caps(GstCaps *caps);

} // namespace genesis::adapters::engine::ges
