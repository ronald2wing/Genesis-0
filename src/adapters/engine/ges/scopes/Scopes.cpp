// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "adapters/engine/ges/scopes/Scopes.h"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <gst/gl/egl/gstgldisplay_egl.h>
#include <gst/gl/gl.h>
#include <gst/gl/gstglfuncs.h>
#include <mutex>
#include <thread>
#include <utility>

namespace genesis::adapters::engine::ges {

namespace {

// Rec.709 luma of a display byte triple, rounded to a display byte. Matches
// the Concat counting shader's coefficients, not Rec.601.
std::uint8_t luma_level(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    const float y = 0.2126f * static_cast<float>(r) + 0.7152f * static_cast<float>(g)
            + 0.0722f * static_cast<float>(b);
    const int rounded = static_cast<int>(y + 0.5f);
    return static_cast<std::uint8_t>(rounded > 255 ? 255 : rounded);
}

// The Concat plane mapping: level_of(v) = min(u32(clamp(v,0,1)*255+0.5),255).
std::uint32_t level_of(float v)
{
    const float scaled = v * 255.0f + 0.5f;
    if (scaled <= 0.0f) {
        return 0;
    }
    if (scaled >= 255.0f) {
        return 255;
    }
    return static_cast<std::uint32_t>(scaled);
}

// The Cb/Cr plane coordinates of a display byte triple, as a (column, row)
// pair into the 256x256 vectorscope; row 0 is the top (+Cr). Matches the
// Concat gpu.rs plane math: cb=(e.b-y)/1.8556, cr=(e.r-y)/1.5748.
void vectorscope_plane(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint32_t &col,
                       std::uint32_t &row)
{
    const float er = static_cast<float>(r) / 255.0f;
    const float eg = static_cast<float>(g) / 255.0f;
    const float eb = static_cast<float>(b) / 255.0f;
    const float y = 0.2126f * er + 0.7152f * eg + 0.0722f * eb;
    const float cb = (eb - y) / 1.8556f;
    const float cr = (er - y) / 1.5748f;
    col = level_of(cb + 0.5f);
    row = level_of(0.5f - cr);
}

// The GL texture target, producer sync point, and first GstGLMemory carried by
// the engine's buffer. The GES adapter passes a GstBuffer ref as `keep_alive`
// (its within-adapter contract), so the void pointer is that GstBuffer by
// construction - it is not validated with GST_IS_BUFFER here, because that
// check dereferences an arbitrary pointer and would read past whatever it
// actually points at. Peek the first GstGLMemory for the target and the memory
// itself, and the buffer for the GstGLSyncMeta glcolorconvert sets on its
// output. A zero target or a null meta means "not a GL buffer" or "no sync
// point was set".
struct BufferGlInfo
{
    std::uint32_t texture_target = 0;
    GstGLSyncMeta *sync_meta = nullptr;
    GstMemory *gl_memory = nullptr; // borrowed from keep_alive's buffer
};

BufferGlInfo buffer_gl_info(const std::shared_ptr<void> &keep_alive)
{
    BufferGlInfo info;
    if (keep_alive == nullptr) {
        return info;
    }
    GstBuffer *buffer = static_cast<GstBuffer *>(keep_alive.get());
    GstMemory *memory = gst_buffer_peek_memory(buffer, 0);
    if (memory != nullptr && gst_is_gl_memory(memory)) {
        info.gl_memory = memory;
        info.texture_target = gst_gl_texture_target_to_gl(
                gst_gl_memory_get_texture_target(GST_GL_MEMORY_CAST(memory)));
    }
    info.sync_meta = gst_buffer_get_gl_sync_meta(buffer);
    return info;
}

} // namespace

ScopeFrame reduce_scopes(const std::uint8_t *rgba, std::uint32_t width, std::uint32_t height)
{
    ScopeFrame frame;
    Histogram &hist = frame.histogram;
    Waveform &wave = frame.waveform;
    Vectorscope &vec = frame.vectorscope;

    wave.columns = kScopeColumns;
    wave.red_min.fill(255);
    wave.green_min.fill(255);
    wave.blue_min.fill(255);
    wave.luma_min.fill(255);

    vec.size = kScopeLevels;
    vec.density.assign(static_cast<std::size_t>(kScopeLevels) * kScopeLevels, 0);

    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t *row = rgba + static_cast<std::size_t>(y) * width * 4;
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint8_t r = row[0];
            const std::uint8_t g = row[1];
            const std::uint8_t b = row[2];
            row += 4;

            ++hist.red[r];
            ++hist.green[g];
            ++hist.blue[b];
            const std::uint8_t luma = luma_level(r, g, b);
            ++hist.luma[luma];

            const std::uint32_t col = static_cast<std::uint32_t>(static_cast<std::uint64_t>(x)
                                                                 * kScopeColumns / width);
            wave.red_min[col] = std::min(wave.red_min[col], r);
            wave.red_max[col] = std::max(wave.red_max[col], r);
            wave.green_min[col] = std::min(wave.green_min[col], g);
            wave.green_max[col] = std::max(wave.green_max[col], g);
            wave.blue_min[col] = std::min(wave.blue_min[col], b);
            wave.blue_max[col] = std::max(wave.blue_max[col], b);
            wave.luma_min[col] = std::min(wave.luma_min[col], luma);
            wave.luma_max[col] = std::max(wave.luma_max[col], luma);

            std::uint32_t vc = 0;
            std::uint32_t vr = 0;
            vectorscope_plane(r, g, b, vc, vr);
            ++vec.density[static_cast<std::size_t>(vr) * kScopeLevels + vc];
        }
    }
    return frame;
}

// ---- non-blocking GL readback -------------------------------------------
//
// The preferred Scopes(GstGLContext*) reads on the pipeline's own context: its
// already-running GL thread and filled vtable do the work, and the textures it
// reads are the very ones the pipeline painted, so nothing can come back blank.
// The legacy Scopes(GlHandles) wraps the app's raw EGL handles - a wrapped
// GstGLContext has no vtable of its own (its activate() only records the
// thread), so Scopes asks the display for a real context that shares them, the
// same dance the GL elements perform internally, which yields a filled vtable
// and a running GL thread.
//
// The double-buffered PBO + fence readback is the one the spike proved
// (spikes/gl-working-space/glscopes.c): queue the current frame into one PBO
// (glReadPixels returns at once), and map the other PBO once its fence has
// signaled.
//
// read_back itself never waits on anything: it hands the frame to a dedicated
// worker and returns the most recently completed scopes. That worker is the
// only caller of gst_gl_context_thread_add, so the synchronous marshalling,
// the fence wait, and the ~2 ms reduce_scopes all land on the worker rather
// than the engine's streaming thread (an inline reduction + thread_add there
// collapsed the render path by 4.5x). The worker may block on the GPU waiting
// for a fence - the streaming thread never does, which is the whole point. A
// single pending-task slot means an unread frame is dropped when a newer one
// lands - a monitor only needs the most recent frame.

// A pending readback: the frame to read, the GL target to attach, the
// producer's sync point to wait on, and the token that keeps the texture alive
// until the readback is consumed.
struct ReadbackTask
{
    std::uint64_t texture_id = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t texture_target = 0; // 0 = derive/fallback to GL_TEXTURE_2D
    // Borrowed from the buffer in `keep_alive`, which owns the meta.
    GstGLSyncMeta *sync_meta = nullptr;
    // The frame's first GstGLMemory, borrowed from `keep_alive`. Non-null only
    // when the engine handed us a GL buffer; materialized before the read.
    GstMemory *gl_memory = nullptr;
    // A private owner->reader fence for a materialized upload on a context
    // distinct from the reader, set on the owner after the upload. Created by
    // the worker; the private buffer that owns it is dropped after the readback
    // returns. Null when the upload ran on the reader's own context (serialized
    // by the owner's GL thread) or there was no upload.
    GstGLSyncMeta *upload_sync_meta = nullptr;
    std::shared_ptr<void> keep_alive;
};

struct Scopes::Impl
{
    // The pipeline's own context (ref'd), or a sharing context created from
    // raw handles by the legacy constructor. Either way it has a filled vtable
    // and a running GL thread for the readback to marshal onto.
    GstGLContext *context = nullptr;
    const GstGLFuncs *gl = nullptr;

    // GL state, touched only on the GL thread inside the readback phase.
    guint pbo[2] = { 0, 0 };
    // Bytes allocated for each PBO slot, tracked per slot so a frame-size
    // change reallocates only the slot about to be written, never the slot
    // whose readback is still pending.
    gsize pbo_size[2] = { 0, 0 };
    guint fbo = 0;
    gint pbo_idx = 0;
    GLsync fence[2] = { nullptr, nullptr };
    std::shared_ptr<void> keep_alive[2];
    std::uint32_t width[2] = { 0, 0 };
    std::uint32_t height[2] = { 0, 0 };

    // Raw bytes the readback phase copies out of the completed PBO. The worker
    // reads them only after the synchronous thread_add has returned.
    std::vector<std::uint8_t> raw_bytes;
    std::uint32_t raw_width = 0;
    std::uint32_t raw_height = 0;
    bool raw_ready = false;

    // The worker owns the readback + reduction, so read_back never blocks on
    // either. A single pending-task slot drops an unread frame when a newer
    // one lands.
    std::thread worker;
    std::mutex task_mutex;
    std::condition_variable task_cv;
    std::optional<ReadbackTask> pending_task;
    bool shutdown = false;

    std::mutex mutex;
    bool has_latest = false;
    ScopeFrame latest;

    // Readback counters and last-attempt state for readback_stats(). Guarded by
    // its own mutex: written on the GL thread and the worker, read anywhere.
    std::mutex stats_mutex;
    ReadbackStats stats;
};

namespace {

// What readback_on_gl receives: the state it mutates and the task it reads.
struct ReadbackPhase
{
    Scopes::Impl *impl;
    ReadbackTask *task;
};

void readback_on_gl(GstGLContext *context, gpointer user);

// The readback worker: consumes the latest pending task, drives the readback on
// the GL thread, then reduces the completed bytes and publishes the ScopeFrame.
// Being the only caller of thread_add, it absorbs the synchronous marshalling
// and the full-frame reduction so the streaming thread never pays for either.
void readback_worker(Scopes::Impl *impl)
{
    std::unique_lock<std::mutex> lock(impl->task_mutex);
    for (;;) {
        impl->task_cv.wait(lock,
                           [impl] { return impl->pending_task.has_value() || impl->shutdown; });
        if (impl->shutdown) {
            return;
        }
        ReadbackTask task = std::move(*impl->pending_task);
        impl->pending_task.reset();
        lock.unlock();

        // Materialize a WRAP_SYSMEM GstGLMemory before the GL thread samples
        // it. Raw-data glupload wraps CPU bytes into a GL memory whose texture
        // is only allocated (TexImage2D(NULL)) with TRANSFER_NEED_UPLOAD still
        // set; a RGBA->RGBA glcolorconvert can pass it through unmapped,
        // leaving the texture empty. The GST_MAP_READ|GST_MAP_GL map is the one
        // thing that uploads the CPU bytes (glTexSubImage2D) and clears
        // NEED_UPLOAD, so it must run before the FBO read - otherwise the
        // readback comes back black. The map marshals onto the memory's owning
        // context, so it runs here on the worker, never inside the consumer
        // callback on that same thread. GPU-backed memories carry no NEED_UPLOAD
        // flag and stay on the GPU untouched.
        GstMemory *gl_memory = task.gl_memory;
        const bool needs_upload = gl_memory != nullptr
                && GST_MEMORY_FLAG_IS_SET(gl_memory, GST_GL_BASE_MEMORY_TRANSFER_NEED_UPLOAD);
        GstMapInfo map_info = GST_MAP_INFO_INIT;
        bool mapped = false;
        if (needs_upload) {
            mapped = gst_memory_map(gl_memory, &map_info,
                                    static_cast<GstMapFlags>(GST_MAP_READ | GST_MAP_GL))
                    != FALSE;
        }
        if (needs_upload && !mapped) {
            // The texture still holds empty storage; a read would publish
            // black. Record the failure and publish nothing.
            {
                std::lock_guard<std::mutex> stats_lock(impl->stats_mutex);
                ++impl->stats.processed;
                ++impl->stats.materialize_failed;
            }
            lock.lock();
            continue;
        }

        // The map uploads the CPU bytes on the memory's OWNING context, which
        // can differ from the readback context. The producer's own sync meta
        // can predate this upload, so it does not order it. For a cross-context
        // upload, set a private sync point on the owner AFTER the upload (the
        // owner's GL thread serialises the two) and have the reader wait on it
        // before sampling. An owner that provably cannot share with the reader
        // would leave the upload unreadable - treat it as a materialize failure
        // rather than sample empty or undefined storage.
        GstBuffer *upload_sync_buffer = nullptr;
        if (mapped) {
            GstGLContext *owner = GST_GL_MEMORY_CAST(gl_memory)->mem.context;
            if (owner != impl->context) {
                if (owner == nullptr || !gst_gl_context_can_share(impl->context, owner)) {
                    gst_memory_unmap(gl_memory, &map_info);
                    {
                        std::lock_guard<std::mutex> stats_lock(impl->stats_mutex);
                        ++impl->stats.processed;
                        ++impl->stats.materialize_failed;
                    }
                    lock.lock();
                    continue;
                }
                upload_sync_buffer = gst_buffer_new();
                task.upload_sync_meta = gst_buffer_add_gl_sync_meta(owner, upload_sync_buffer);
                if (task.upload_sync_meta == nullptr) {
                    // Cannot order the upload against the read; the fence is
                    // the only guarantee the bytes reached the texture first.
                    gst_buffer_unref(upload_sync_buffer);
                    upload_sync_buffer = nullptr;
                    gst_memory_unmap(gl_memory, &map_info);
                    {
                        std::lock_guard<std::mutex> stats_lock(impl->stats_mutex);
                        ++impl->stats.processed;
                        ++impl->stats.materialize_failed;
                    }
                    lock.lock();
                    continue;
                }
                // Runs on the owner's GL thread, after the upload above: the
                // fence is inserted at the end of the upload's command stream.
                gst_gl_sync_meta_set_sync_point(task.upload_sync_meta, owner);
            }
        }

        ReadbackPhase phase{ impl, &task };
        gst_gl_context_thread_add(impl->context, readback_on_gl, &phase);
        {
            std::lock_guard<std::mutex> stats_lock(impl->stats_mutex);
            ++impl->stats.processed;
        }

        if (mapped) {
            // The readback has sampled the texture; release the map back to the
            // memory so the buffer it borrows from can be recycled.
            gst_memory_unmap(gl_memory, &map_info);
        }
        if (upload_sync_buffer != nullptr) {
            // Drop the private sync meta (and its fence, freed on the owner's
            // thread); the frame's buffer still holds the owner context alive.
            gst_buffer_unref(upload_sync_buffer);
        }

        if (impl->raw_ready) {
            ScopeFrame frame =
                    reduce_scopes(impl->raw_bytes.data(), impl->raw_width, impl->raw_height);
            impl->raw_ready = false;
            std::lock_guard<std::mutex> latest_lock(impl->mutex);
            impl->latest = std::move(frame);
            impl->has_latest = true;
            std::lock_guard<std::mutex> stats_lock(impl->stats_mutex);
            ++impl->stats.completed;
        }

        lock.lock();
    }
}

// Runs on the GL thread. Queues this frame's readback into one PBO, then maps
// the other PBO once its fence has signaled (waiting up to 50 ms for the GPU -
// the worker, not the streaming thread, pays this). The mapped bytes are copied
// out for the worker to reduce - never reduced here.
void readback_on_gl(GstGLContext * /*context*/, gpointer user)
{
    auto *phase = static_cast<ReadbackPhase *>(user);
    Scopes::Impl *impl = phase->impl;
    ReadbackTask &task = *phase->task;
    const GstGLFuncs *gl = impl->gl;
    const std::uint32_t width = task.width;
    const std::uint32_t height = task.height;
    const gsize size = static_cast<gsize>(width) * height * 4;

    // The readback runs on a context the caller also uses (the pipeline's, or
    // the app's via the legacy constructor), so leave the GL state exactly as
    // found. Save the slice this pass mutates - including the PBO allocation
    // below - and restore it on every exit.
    const bool state_saved = gl->GetIntegerv != nullptr;
    // GLES2/desktop GL < 3.0 have a single framebuffer binding; GLES3/desktop
    // GL >= 3.0 split it into read and draw. Bind and restore whichever pair
    // the context actually exposes so the app's draw framebuffer survives us.
    const bool separate_read_draw = gst_gl_context_check_gl_version(
            impl->context,
            static_cast<GstGLAPI>(GST_GL_API_OPENGL | GST_GL_API_OPENGL3 | GST_GL_API_GLES2), 3, 0);
    GLint saved_pack_alignment = 4;
    GLint saved_pack_row_length = 0;
    GLint saved_pack_buffer = 0;
    GLint saved_read_framebuffer = 0;
    GLint saved_draw_framebuffer = 0;
    if (state_saved) {
        gl->GetIntegerv(GL_PACK_ALIGNMENT, &saved_pack_alignment);
        gl->GetIntegerv(GL_PACK_ROW_LENGTH, &saved_pack_row_length);
        gl->GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &saved_pack_buffer);
        if (separate_read_draw) {
            gl->GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &saved_read_framebuffer);
            gl->GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &saved_draw_framebuffer);
        } else {
            gl->GetIntegerv(GL_FRAMEBUFFER_BINDING, &saved_draw_framebuffer);
            saved_read_framebuffer = saved_draw_framebuffer;
        }
    }
    const auto restore_state = [&] {
        if (!state_saved) {
            return;
        }
        if (separate_read_draw) {
            gl->BindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(saved_read_framebuffer));
            gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(saved_draw_framebuffer));
        } else {
            gl->BindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(saved_draw_framebuffer));
        }
        gl->BindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(saved_pack_buffer));
        gl->PixelStorei(GL_PACK_ALIGNMENT, saved_pack_alignment);
        gl->PixelStorei(GL_PACK_ROW_LENGTH, saved_pack_row_length);
    };

    // Lazily allocate the two pixel-pack PBOs and the read FBO on first use,
    // and grow them if the frame size ever changes. Only the slot being written
    // this pass is resized: the other slot may still hold a pending readback at
    // its old size, so touching it would clobber the frame it is about to hand
    // back.
    if (impl->pbo[0] == 0 && gl->GenBuffers != nullptr) {
        gl->GenBuffers(2, impl->pbo);
        impl->pbo_size[0] = 0;
        impl->pbo_size[1] = 0;
    }
    const int cur = impl->pbo_idx;
    const int prev = cur ^ 1;
    if (impl->pbo[0] != 0 && impl->pbo_size[cur] != size) {
        gl->BindBuffer(GL_PIXEL_PACK_BUFFER, impl->pbo[cur]);
        gl->BufferData(GL_PIXEL_PACK_BUFFER, size, nullptr, GL_STREAM_READ);
        gl->BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        impl->pbo_size[cur] = size;
    }
    if (impl->fbo == 0 && gl->GenFramebuffers != nullptr) {
        gl->GenFramebuffers(1, &impl->fbo);
    }
    if (impl->pbo[0] == 0 || impl->fbo == 0) {
        restore_state();
        return;
    }

    const std::uint32_t target = task.texture_target != 0 ? task.texture_target : GL_TEXTURE_2D;

    const bool has_sync = task.sync_meta != nullptr;
    // glWaitSync on a fence from a context outside our share group is
    // undefined, so only wait when the producer provably shares ours.
    // can_share is GStreamer's best-effort answer (it reports false for a pair
    // of wrapped contexts), so an unconfirmed pair skips the wait rather than
    // issue the undefined call.
    const bool sync_shared = has_sync && task.sync_meta->context != nullptr
            && gst_gl_context_can_share(impl->context, task.sync_meta->context);

    // The producer's draw and our ReadPixels are two command streams on two
    // sharing contexts with no mutual ordering. Wait on the producer's fence -
    // a GPU-side glWaitSync GStreamer inserts for us - so the ReadPixels below
    // cannot execute before the texture has been written.
    if (sync_shared) {
        gst_gl_sync_meta_wait(task.sync_meta, impl->context);
    }

    // A materialized upload on a distinct owner context is a new write that the
    // producer's sync meta (which can predate it) does not order. Wait on the
    // private owner->reader fence set after the upload, so the ReadPixels below
    // cannot execute before the glTexSubImage2D upload completes. The owner was
    // checked to share the reader when the fence was created, so the glWaitSync
    // is defined.
    if (task.upload_sync_meta != nullptr) {
        gst_gl_sync_meta_wait(task.upload_sync_meta, impl->context);
        std::lock_guard<std::mutex> stats_lock(impl->stats_mutex);
        ++impl->stats.materialize_synced;
    }

    // Queue this frame's readback into the current PBO.
    gl->BindFramebuffer(GL_FRAMEBUFFER, impl->fbo);

    // A stale error must not be mistaken for this attachment's: drain first,
    // then require both a clean GetError and a complete framebuffer before
    // reading. (The GetError check is why the texture must live in our
    // context.)
    if (gl->GetError != nullptr) {
        for (int i = 0; i < 8 && gl->GetError() != GL_NO_ERROR; ++i) { }
    }
    gl->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target,
                             static_cast<GLuint>(task.texture_id), 0);
    const GLenum attach_error = gl->GetError != nullptr ? gl->GetError() : GL_NO_ERROR;
    const bool fbo_complete = gl->CheckFramebufferStatus != nullptr
            && gl->CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (attach_error != GL_NO_ERROR || !fbo_complete) {
        // Wrong target or an unusable texture: nothing was read. Leave the PBO
        // index alone so the next frame still lines up, and publish no frame -
        // a failed read must not masquerade as black.
        restore_state();
        std::lock_guard<std::mutex> stats_lock(impl->stats_mutex);
        ++impl->stats.failed;
        impl->stats.last_target = target;
        impl->stats.last_fbo_complete = fbo_complete;
        impl->stats.last_sync_meta = has_sync;
        impl->stats.sync_context_shared = sync_shared;
        return;
    }

    gl->BindBuffer(GL_PIXEL_PACK_BUFFER, impl->pbo[cur]);
    gl->PixelStorei(GL_PACK_ALIGNMENT, 1);
    gl->PixelStorei(GL_PACK_ROW_LENGTH, 0);
    gl->ReadPixels(0, 0, static_cast<GLint>(width), static_cast<GLint>(height), GL_RGBA,
                   GL_UNSIGNED_BYTE, nullptr);
    gl->BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    {
        std::lock_guard<std::mutex> stats_lock(impl->stats_mutex);
        impl->stats.last_target = target;
        impl->stats.last_fbo_complete = true;
        impl->stats.last_sync_meta = has_sync;
        impl->stats.sync_context_shared = sync_shared;
    }
    if (impl->fence[cur] != nullptr && gl->DeleteSync != nullptr) {
        gl->DeleteSync(impl->fence[cur]);
    }
    impl->fence[cur] =
            gl->FenceSync != nullptr ? gl->FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0) : nullptr;
    impl->width[cur] = width;
    impl->height[cur] = height;
    impl->keep_alive[cur] = std::move(task.keep_alive);

    // Complete the previous frame's readback: wait for its fence (the worker
    // may block on the GPU, the streaming thread never does), then copy the
    // bytes out for the worker to reduce.
    if (impl->fence[prev] != nullptr && gl->ClientWaitSync != nullptr
        && gl->MapBufferRange != nullptr) {
        const GLenum wait = gl->ClientWaitSync(impl->fence[prev], GL_SYNC_FLUSH_COMMANDS_BIT,
                                               50000000ULL); // 50 ms
        if (wait == GL_ALREADY_SIGNALED || wait == GL_CONDITION_SATISFIED) {
            gl->DeleteSync(impl->fence[prev]);
            impl->fence[prev] = nullptr;
            // Map exactly what this slot was allocated for, not the incoming
            // frame's size: the two are equal while sizes are stable, but after
            // a size change the slot still holds its old-size readback and its
            // allocation is the source of truth.
            const gsize prev_size = impl->pbo_size[prev];
            gl->BindBuffer(GL_PIXEL_PACK_BUFFER, impl->pbo[prev]);
            void *ptr = gl->MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, prev_size, GL_MAP_READ_BIT);
            if (ptr != nullptr) {
                impl->raw_bytes.resize(prev_size);
                std::memcpy(impl->raw_bytes.data(), ptr, prev_size);
                impl->raw_width = impl->width[prev];
                impl->raw_height = impl->height[prev];
                impl->raw_ready = true;
                gl->UnmapBuffer(GL_PIXEL_PACK_BUFFER);
            }
            gl->BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            impl->keep_alive[prev].reset();
        }
    }

    restore_state();
    impl->pbo_idx = prev;
}

// Deletes the GL resources on the GL thread, while the context is still alive.
struct CleanupTask
{
    const GstGLFuncs *gl;
    guint pbo[2];
    guint fbo;
    GLsync fence[2];
};

void cleanup_on_gl(GstGLContext * /*context*/, gpointer user)
{
    auto *task = static_cast<CleanupTask *>(user);
    const GstGLFuncs *gl = task->gl;
    if (gl == nullptr) {
        return;
    }
    for (int i = 0; i < 2; ++i) {
        if (task->fence[i] != nullptr && gl->DeleteSync != nullptr) {
            gl->DeleteSync(task->fence[i]);
        }
    }
    if (task->pbo[0] != 0 && gl->DeleteBuffers != nullptr) {
        gl->DeleteBuffers(2, task->pbo);
    }
    if (task->fbo != 0 && gl->DeleteFramebuffers != nullptr) {
        gl->DeleteFramebuffers(1, &task->fbo);
    }
}

} // namespace

Scopes::Scopes(engine::GlHandles handles) : impl_(std::make_unique<Impl>())
{
    // The readback worker lives for the whole Scopes lifetime and idles until
    // a frame is handed to it. Spawned first so every construction/destruction
    // pair is symmetric, even for an inert Scopes with no GL context.
    impl_->worker = std::thread(readback_worker, impl_.get());

    if (handles.display == 0 || handles.context == 0) {
        return; // inert: read_back returns nullopt
    }

    GstGLDisplay *display = reinterpret_cast<GstGLDisplay *>(
            gst_gl_display_egl_new_with_egl_display(reinterpret_cast<gpointer>(handles.display)));
    if (display == nullptr) {
        return;
    }
    GstGLContext *wrapped = gst_gl_context_new_wrapped(
            display, static_cast<guintptr>(handles.context), GST_GL_PLATFORM_EGL, GST_GL_API_GLES2);
    if (wrapped == nullptr) {
        gst_object_unref(display);
        return;
    }

    GError *error = nullptr;
    const gboolean created =
            gst_gl_display_create_context(display, wrapped, &impl_->context, &error);
    gst_object_unref(wrapped);
    gst_object_unref(display); // the real context holds the display alive
    if (!created) {
        g_clear_error(&error);
        return;
    }
    impl_->gl = impl_->context->gl_vtable;
}

Scopes::Scopes(GstGLContext *context) : impl_(std::make_unique<Impl>())
{
    // Symmetric with the GlHandles constructor: the readback worker lives for
    // the whole Scopes lifetime and idles until a frame is handed to it.
    impl_->worker = std::thread(readback_worker, impl_.get());

    if (context == nullptr) {
        return; // inert: read_back returns nullopt
    }

    // Read back on the pipeline's own context. Its GL thread and filled vtable
    // do the work, and the textures it reads are the ones the pipeline painted,
    // so nothing can come back blank. Ref it for our lifetime; the caller keeps
    // its own reference.
    impl_->context = static_cast<GstGLContext *>(gst_object_ref(context));
    impl_->gl = context->gl_vtable;
}

Scopes::~Scopes()
{
    if (impl_ == nullptr) {
        return;
    }
    // Stop the readback worker before releasing the GL context, so it can
    // never touch state the context teardown is about to free.
    {
        std::lock_guard<std::mutex> lock(impl_->task_mutex);
        impl_->shutdown = true;
    }
    impl_->task_cv.notify_one();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }

    if (impl_->context == nullptr) {
        return;
    }
    // Drop the engine's buffers while their GL contexts are still alive: the
    // buffers own the GL memory (and, through their GstGLMemory, a ref to the
    // producing context), so releasing them frees that memory on a live context
    // instead of after impl_->context is gone.
    impl_->keep_alive[0].reset();
    impl_->keep_alive[1].reset();
    if (impl_->gl != nullptr) {
        CleanupTask task{ impl_->gl,
                          { impl_->pbo[0], impl_->pbo[1] },
                          impl_->fbo,
                          { impl_->fence[0], impl_->fence[1] } };
        gst_gl_context_thread_add(impl_->context, cleanup_on_gl, &task);
    }
    gst_object_unref(impl_->context);
}

std::optional<ScopeFrame> Scopes::read_back(std::uint64_t texture_id, std::uint32_t width,
                                            std::uint32_t height, std::shared_ptr<void> keep_alive,
                                            std::uint32_t texture_target)
{
    if (impl_ == nullptr || impl_->context == nullptr || impl_->gl == nullptr) {
        return std::nullopt;
    }
    if (texture_id == 0 || width == 0 || height == 0) {
        return std::nullopt;
    }

    // Recover the attach target, the producer's sync point, and the GL memory
    // from the engine's buffer before it moves into the task. An explicit
    // texture_target wins.
    std::uint32_t target = texture_target;
    GstGLSyncMeta *sync_meta = nullptr;
    GstMemory *gl_memory = nullptr;
    if (keep_alive != nullptr) {
        const BufferGlInfo info = buffer_gl_info(keep_alive);
        if (target == 0) {
            target = info.texture_target;
        }
        sync_meta = info.sync_meta;
        gl_memory = info.gl_memory;
    }

    // Hand the frame to the worker (latest wins) and return the most recently
    // completed scopes - without waiting on the GPU or the reduction.
    {
        std::lock_guard<std::mutex> lock(impl_->task_mutex);
        impl_->pending_task = ReadbackTask{ texture_id, width,     height,  target,
                                            sync_meta,  gl_memory, nullptr, std::move(keep_alive) };
    }
    {
        std::lock_guard<std::mutex> lock(impl_->stats_mutex);
        ++impl_->stats.submitted;
    }
    impl_->task_cv.notify_one();

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->has_latest) {
        return std::nullopt;
    }
    return impl_->latest;
}

std::optional<ScopeFrame> Scopes::latest() const
{
    if (impl_ == nullptr) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->has_latest) {
        return std::nullopt;
    }
    return impl_->latest;
}

ReadbackStats Scopes::readback_stats() const
{
    if (impl_ == nullptr) {
        return { };
    }
    std::lock_guard<std::mutex> lock(impl_->stats_mutex);
    return impl_->stats;
}

std::optional<ScopeFrame> read_scopes(Scopes &scopes)
{
    return scopes.latest();
}

} // namespace genesis::adapters::engine::ges
