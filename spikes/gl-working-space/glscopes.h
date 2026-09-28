// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// glscopes: a GstGLFilter subclass that passes its input through unchanged
// while also reading the frame back for a 256-bin luma histogram, WITHOUT
// stalling the GL pipeline. This is the non-blocking readback candidate for
// the monitor's histogram/waveform scopes.
//
// How it avoids the stall: instead of a synchronous glReadPixels (what
// gldownload does), it double-buffers two GL_PIXEL_PACK_BUFFER objects and
// glReadPixels into the CURRENT one (which returns immediately after queueing
// the DMA), then maps the PREVIOUS frame's PBO — whose transfer is already
// complete — guarded by a fence so glMapBufferRange never blocks on the GPU.
//
// The element is a GstGLFilter (single input, single output, RGBA GLMemory)
// only so it can sit on a tee branch next to the main render path; its output
// is discarded (a fakesink). The histogram is accumulated CPU-side from the
// mapped PBO and printed on GL stop.

#ifndef GL_SCOPES_H
#define GL_SCOPES_H

#include <gst/gst.h>

G_BEGIN_DECLS

GType gst_gl_scopes_get_type (void);

G_END_DECLS

#endif  // GL_SCOPES_H
