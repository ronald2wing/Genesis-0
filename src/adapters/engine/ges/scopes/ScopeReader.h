// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "adapters/engine/EngineSession.h"
#include "adapters/engine/ScopeTypes.h"

// The pipeline's GL context is an opaque forward-declared type here, not a
// <gst/gl/gl.h> include: only the adapter's .cpp pulls in the real GL headers.
typedef struct _GstGLContext GstGLContext;

namespace genesis::adapters::engine::ges {

// The engine-neutral scope types live on the seam; re-exported here so the
// adapter's own code and tests keep their short `ges::` names.
using engine::Histogram;
using engine::is_hdr_signal;
using engine::kScopeColumns;
using engine::kScopeLevels;
using engine::scope_scale_max;
using engine::ScopeFrame;
using engine::ScopeSignal;
using engine::Vectorscope;
using engine::Waveform;

// Counters and last-attempt state for the GL readback, so a blank readback can
// be told apart from a wrong target, an incomplete framebuffer, or a missing
// producer sync point without a debugger. Plain integers/bools only: the header
// stays GStreamer-free.
struct ReadbackStats
{
    // Frames handed to the worker by read_back. Not a readback count: a busy
    // worker drops an unread frame (latest wins), so this can exceed processed.
    std::uint64_t submitted = 0;
    // Frames the worker consumed: a readback attempt (success or failure) or a
    // materialize failure. Not a publish count: the readback is one frame
    // behind, so this can exceed completed.
    std::uint64_t processed = 0;
    // Frames reduced and published.
    std::uint64_t completed = 0;
    // Incomplete FBO / bad target / GL error: nothing read, nothing published.
    std::uint64_t failed = 0;
    // A WRAP_SYSMEM frame whose CPU bytes never became readable on the readback
    // context: the GST_MAP_READ|GST_MAP_GL map failed (empty storage), the
    // owner context provably cannot share with the reader, or the owner->reader
    // fence could not be created. Nothing read, nothing published.
    std::uint64_t materialize_failed = 0;
    // A WRAP_SYSMEM frame whose upload ran on an owner context distinct from
    // the reader, and whose private owner->reader fence was established: the
    // sync point was set on the owner after the upload and the reader waited on
    // it before sampling.
    std::uint64_t materialize_synced = 0;
    std::uint32_t last_target = 0; // GL target used for the last processed frame
    bool last_fbo_complete = false; // last processed frame had a complete FBO
    bool last_sync_meta = false; // producer sync point was found on the buffer
    bool sync_context_shared = false; // producer context is in our share group
};

// Reduces a display-encoded RGBA8 frame (width*height*4 tightly packed bytes)
// to one frame's scopes. Pure and deterministic - no GL state - so the numeric
// behaviour is unit-testable without a display.
ScopeFrame reduce_scopes(const std::uint8_t *rgba, std::uint32_t width, std::uint32_t height);

// The absolute display luminance in nits of a PQ code value `v` in [0,1]:
// the SMPTE ST 2084 perceptual-quantiser EOTF, clamped to [0, 10000]. v=0
// reads 0 nits, v=1 reads 10000 nits. The single convention every HDR scope
// maps through, so the scope scale is absolute nits rather than a relative
// fraction.
double pq_eotf_nits(double v);

// The display luminance in nits of an HLG signal value `v` in [0,1]: the
// ARIB STD-B67 inverse OETF (signal -> scene light) followed by the BT.2100
// OOTF with a 1000-nit reference display and system gamma 1.2, clamped to
// [0, 1000]. HLG is scene-referred, so the 1000-nit reference is the fixed
// convention this scope pins (a display with a different peak would rescale).
double hlg_eotf_nits(double v);

// Reduces a display-encoded RGBA8 frame to one frame's scopes under `signal`.
// For Sdr this is byte-identical to reduce_scopes: each channel's code value is
// its own bin. For Hlg/Pq each channel value is EOTF-decoded to nits (the two
// functions above), re-quantised to 0..255 against scope_scale_max, and only
// then binned, so the histogram, waveform and parade read on a perceptual nit
// scale rather than a code-value scale. The vectorscope stays on the raw code
// values: chroma is transfer-relative, and a nit mapping would only rescale
// its plane without moving a point. `signal` is recorded on the returned frame
// so downstream consumers (the controller, the snapshot reply) can report it.
ScopeFrame reduce_scopes_signal(const std::uint8_t *rgba, std::uint32_t width, std::uint32_t height,
                                ScopeSignal signal);

// The non-blocking scopes readback, one frame behind the pipeline (PBO +
// fence) so the render path is never stalled. Two constructors:
//
//   ScopeReader(GstGLContext*) - the preferred form. Reads back on the context the
//   engine installs, ref'd for the ScopeReader lifetime; the caller keeps its own
//   reference.
//
//   ScopeReader(GlHandles) - the alternative form, used by the app wiring. Wraps raw
//   EGL handles and asks the display for a context that shares the app's
//   textures. Prefer the GstGLContext constructor.
//
// A readback is only correct when the producer's draw and our ReadPixels can be
// ordered. GStreamer attaches a GstGLSyncMeta fence to its GL output buffers;
// ScopeReader waits on it before sampling, but only when the producer context is
// provably in our share group (gst_gl_context_can_share). That check is
// best-effort - it reports false for a pair of wrapped contexts - so an
// unconfirmed pair skips the wait rather than issue an undefined glWaitSync.
// A texture that is not in our context, a bad target, or an unfenced producer
// draw can still read back black; the readback publishes a frame only when the
// framebuffer check passes, so the failure is visible in readback_stats().
//
// Before sampling, ScopeReader materialises a WRAP_SYSMEM GstGLMemory (raw-data
// glupload wraps CPU bytes whose texture is only allocated, with
// TRANSFER_NEED_UPLOAD still set) by mapping it with GST_MAP_READ|GST_MAP_GL,
// which uploads the bytes and clears the flag. Without that map the texture
// stays empty and the readback comes back black; a map that fails is recorded
// in materialize_failed and publishes nothing.
//
// The map uploads on the memory's OWNING context, which can differ from the
// readback context. The producer's own sync meta can predate that upload, so it
// does not order it. For a cross-context upload ScopeReader sets a private sync
// point on the owner after the upload and waits on it before sampling; an owner
// that provably cannot share with the reader (or whose fence cannot be built)
// is a materialize failure, not an empty-storage read. A same-context upload
// needs no fence: the owner's GL thread serialises the map and the read.
//
// Threading: read_back() runs on the engine's streaming thread and only queues
// the frame before returning the most recently completed scope frame - it never
// calls into GL. All GL work (PBO + fence readback, cleanup) runs on the
// context's own GL thread via gst_gl_context_thread_add, which GStreamer
// serialises per context, so no second thread ever races a render or calls
// eglMakeCurrent concurrently on that context. The mechanism is in ScopeReader.cpp.
class ScopeReader
{
public:
    explicit ScopeReader(GstGLContext *context);
    explicit ScopeReader(engine::GlHandles handles);
    ~ScopeReader();
    ScopeReader(const ScopeReader &) = delete;
    ScopeReader &operator=(const ScopeReader &) = delete;

    // Submits a non-blocking readback of `texture_id` (an RGBA8 texture in the
    // adopted context) to the internal worker, which reads it back one frame
    // behind the pipeline (PBO + fence) and reduces it. Returns the most
    // recently completed scope frame, or nullopt until the first readback
    // completes. `keep_alive` keeps the engine's buffer - and so the texture -
    // alive until its readback is consumed. The worker recovers the GL target
    // and the producer's GstGLSyncMeta from that buffer: the GES adapter passes
    // a GstBuffer ref as its within-adapter contract. Never blocks on the GPU
    // or the reduction: the caller is the engine's streaming thread, and all
    // waiting happens on the worker. Call once per presented frame.
    //
    // `texture_target` is the GL texture target to attach (e.g. GL_TEXTURE_2D).
    // Zero derives it from the buffer's GstGLMemory, falling back to
    // GL_TEXTURE_2D; a non-zero value overrides the derivation (test seam and
    // the RECTANGLE/external-OES path).
    std::optional<ScopeFrame> read_back(std::uint64_t texture_id, std::uint32_t width,
                                        std::uint32_t height,
                                        std::shared_ptr<void> keep_alive = { },
                                        std::uint32_t texture_target = 0,
                                        ScopeSignal signal = ScopeSignal::Sdr);
    // The most recently completed scope frame, or nullopt. Any thread.
    std::optional<ScopeFrame> latest() const;

    // Counters and last-attempt state for the readback. Any thread.
    ReadbackStats readback_stats() const;

    // Public but incomplete: the definition lives in ScopeReader.cpp, so nothing
    // outside that translation unit can instantiate or touch it.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// The engine-neutral scopes read: the previous frame's completed data (never
// blocks); nullopt until a readback has completed.
std::optional<ScopeFrame> read_scopes(ScopeReader &scopes);

} // namespace genesis::adapters::engine::ges
