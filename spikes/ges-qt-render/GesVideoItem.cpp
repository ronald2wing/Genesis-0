// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "GesVideoItem.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <QDebug>
#include <QOpenGLContext>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QSurfaceFormat>
#include <QTimer>
#include <QUrl>

#include <gst/video/video.h>
#include <gst/gl/egl/gstgldisplay_egl.h>
#include <gst/gl/gstglcontext.h>
#include <gst/gl/gstglmemory.h>
#include <QtGui/qopenglcontext_platform.h>
#include <rhi/qrhi.h>

GesVideoItem::GesVideoItem(QQuickItem* parent) : QQuickItem(parent) {
    // The item now paints: a scene-graph node is committed per frame.
    setFlag(QQuickItem::ItemHasContents, true);
}

GesVideoItem::~GesVideoItem() {
    std::fprintf(stderr, "[spike] shutdown: observed=%d rendered=%d path='%s'\n",
                 frameCount_, rendered_, framePath_.toUtf8().constData());
    stopPipeline();
    // Render-thread resources are normally released by releaseResources(); at
    // process exit the scene graph may already be gone, so best-effort here.
    releaseResources();
}

void GesVideoItem::setSource(const QString& source) {
    if (source_ == source) return;
    source_ = source;
    emit sourceChanged();
}

void GesVideoItem::componentComplete() {
    QQuickItem::componentComplete();
    std::fprintf(stderr, "[spike] componentComplete, source='%s'\n",
                 source_.toUtf8().constData());
    startPipeline();
}

void GesVideoItem::startPipeline() {
    if (started_ || source_.isEmpty()) {
        std::fprintf(stderr, "[spike] startPipeline skipped (started=%d empty=%d)\n",
                     started_ ? 1 : 0, source_.isEmpty() ? 1 : 0);
        return;
    }

    // SPIKE_CTX=self selects the step-1 self-created EGL context; the default
    // is the step-2 production architecture (wrap Qt's RHI GL context).
    const bool useSelf =
        std::getenv("SPIKE_CTX") != nullptr &&
        std::strcmp(std::getenv("SPIKE_CTX"), "self") == 0;

    if (useSelf) {
        if (!createSelfContext()) return;
    } else {
        // Qt's RHI context exists only after the window's first scene-graph
        // sync; poll for it, bounded, so the spike cannot spin forever.
        if (!wrapQtContext()) return;
    }

    buildAndStartPipeline();
}

void GesVideoItem::deferStart() {
    if (contextAttempts_ >= 120) {
        std::fprintf(stderr, "[spike] gave up waiting for RHI GL context\n");
        framePath_ = QStringLiteral("no RHI GL context after 120 attempts");
        emit framePathChanged();
        return;
    }
    QTimer::singleShot(16, this, [this] { startPipeline(); });
}

bool GesVideoItem::wrapQtContext() {
    QQuickWindow* win = window();
    QOpenGLContext* qctx = nullptr;
    if (win && win->rendererInterface()) {
        qctx = static_cast<QOpenGLContext*>(win->rendererInterface()->getResource(
            win, QSGRendererInterface::OpenGLContextResource));
    }
    if (!qctx) {
        std::fprintf(stderr, "[spike] RHI GL context not ready (attempt %d)\n",
                     contextAttempts_ + 1);
        ++contextAttempts_;
        deferStart();
        return false;
    }
    if (qctx == qtContext_) {
        return true;  // already wrapped
    }

    auto* egl = qctx->nativeInterface<QNativeInterface::QEGLContext>();
    if (!egl) {
        std::fprintf(stderr, "[spike] no QEGLContext native interface\n");
        framePath_ = QStringLiteral("no QEGLContext native interface");
        emit framePathChanged();
        return false;
    }

    // Wrap Qt's EGL display and context so GStreamer's GL elements render into
    // the same share group the scene graph draws from.
    glDisplay_ = reinterpret_cast<GstGLDisplay*>(
        gst_gl_display_egl_new_with_egl_display(egl->display()));
    if (!glDisplay_) {
        std::fprintf(stderr, "[spike] failed to wrap EGL display\n");
        framePath_ = QStringLiteral("failed to wrap EGL display");
        emit framePathChanged();
        return false;
    }

    const GstGLAPI api = qctx->isOpenGLES()
                             ? GST_GL_API_GLES2
                             : (qctx->format().profile() == QSurfaceFormat::CoreProfile
                                    ? GST_GL_API_OPENGL3
                                    : GST_GL_API_OPENGL);
    glContext_ = gst_gl_context_new_wrapped(
        glDisplay_, reinterpret_cast<guintptr>(egl->nativeContext()),
        GST_GL_PLATFORM_EGL, api);
    if (!glContext_) {
        std::fprintf(stderr, "[spike] failed to wrap EGL context\n");
        framePath_ = QStringLiteral("failed to wrap EGL context");
        emit framePathChanged();
        return false;
    }

    qtContext_ = qctx;
    std::fprintf(stderr,
                 "[spike] wrapped Qt RHI context (api=%s gles=%d, %d.%d)\n",
                 qctx->isOpenGLES() ? "gles2" : "gl",
                 qctx->isOpenGLES() ? 1 : 0, qctx->format().majorVersion(),
                 qctx->format().minorVersion());
    return true;
}

bool GesVideoItem::createSelfContext() {
    // GStreamer's GL elements do not create a GL context on their own: the app
    // must provide one. This self-created headless EGL context is the step-1
    // baseline against which the wrapped Qt context is compared.
    glDisplay_ = reinterpret_cast<GstGLDisplay*>(gst_gl_display_egl_new());
    if (!glDisplay_) {
        std::fprintf(stderr, "[spike] no EGL display\n");
        framePath_ = QStringLiteral("no EGL display");
        emit framePathChanged();
        return false;
    }
    glContext_ = gst_gl_context_new(glDisplay_);
    GError* glError = nullptr;
    if (!gst_gl_context_create(glContext_, nullptr, &glError)) {
        std::fprintf(stderr, "[spike] GL context failed: %s\n",
                     glError ? glError->message : "unknown");
        framePath_ = QStringLiteral("GL context failed: %1")
                         .arg(glError ? glError->message : "unknown");
        if (glError) g_error_free(glError);
        emit framePathChanged();
        return false;
    }
    std::fprintf(stderr, "[spike] self-created GL context ready\n");
    return true;
}

void GesVideoItem::buildAndStartPipeline() {
    // Force a software decoder so the decoder choice is not a variable:
    // uridecodebin auto-plugs a hardware decoder (nvdec), which on this host
    // warns "OpenGL context is not CUDA-compatible" and falls back to system
    // memory. A deterministic filesrc ! matroskademux ! vp8dec chain removes
    // that. SPIKE_SINK=none drops glcolorconvert to test glupload ! appsink
    // directly.
    const bool colorconvert =
        std::getenv("SPIKE_SINK") == nullptr ||
        std::strcmp(std::getenv("SPIKE_SINK"), "none") != 0;
    // SPIKE_GL=1 forces the appsink to accept only GLMemory, so glcolorconvert
    // (or glupload, when glcolorconvert is dropped) actually produces a GL
    // texture instead of passing a DMABuf through. Forcing format=RGBA keeps
    // the frame a single 2D texture (I420 would be three planes), which is the
    // one shape the scene graph can present directly.
    const bool glOnly = std::getenv("SPIKE_GL") != nullptr &&
                        std::strcmp(std::getenv("SPIKE_GL"), "1") == 0;
    // filesrc takes a local path, not a URI; the spike sample path has no
    // spaces, so a bare location string is fine.
    const QByteArray desc =
        "filesrc location=" + source_.toUtf8() +
        " ! matroskademux ! vp8dec ! videoconvert ! glupload" +
        (colorconvert ? " ! glcolorconvert" : "") +
        (glOnly
             ? " ! capsfilter caps=\"video/x-raw(memory:GLMemory), format=(string)RGBA\""
             : "") +
        " ! appsink name=sink sync=false max-buffers=2 drop=true";

    GError* error = nullptr;
    pipeline_ = gst_parse_launch(desc.constData(), &error);
    if (!pipeline_) {
        std::fprintf(stderr, "[spike] parse failed: %s\n",
                     error ? error->message : "unknown");
        framePath_ = QStringLiteral("pipeline parse failed: %1")
                         .arg(error ? error->message : "unknown");
        if (error) g_error_free(error);
        emit framePathChanged();
        return;
    }
    std::fprintf(stderr, "[spike] pipeline parsed\n");

    // Elements ask for the GL context over the bus; a sync handler answers
    // before the message is dispatched, the only point early enough for the
    // negotiation to succeed.
    GstBus* bus = gst_element_get_bus(pipeline_);
    gst_bus_set_sync_handler(bus, &GesVideoItem::onBusSync, this, nullptr);
    gst_object_unref(bus);

    sink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "sink");
    GstAppSinkCallbacks callbacks = {};
    callbacks.new_sample = &GesVideoItem::onNewSample;
    gst_app_sink_set_callbacks(GST_APP_SINK(sink_), &callbacks, this, nullptr);

    const GstStateChangeReturn ret =
        gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    std::fprintf(stderr, "[spike] set_state -> %d\n", static_cast<int>(ret));
    started_ = true;
    framePath_ = QStringLiteral("pipeline started, awaiting first frame");
    emit framePathChanged();
}

void GesVideoItem::stopPipeline() {
    if (!started_) return;
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (sink_) {
        gst_object_unref(sink_);
        sink_ = nullptr;
    }
    if (pipeline_) {
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
    if (glContext_) {
        gst_object_unref(glContext_);
        glContext_ = nullptr;
    }
    if (glDisplay_) {
        gst_object_unref(glDisplay_);
        glDisplay_ = nullptr;
    }
    qtContext_ = nullptr;
    started_ = false;
}

GstBusSyncReply GesVideoItem::onBusSync(GstBus* bus, GstMessage* message,
                                        gpointer userData) {
    (void)bus;
    auto* self = static_cast<GesVideoItem*>(userData);
    const GstMessageType msgType = GST_MESSAGE_TYPE(message);

    // Surface everything that matters to the frame-path verdict, since the
    // spike's whole purpose is recording where frames stall or flow.
    switch (msgType) {
    case GST_MESSAGE_ERROR: {
        GError* err = nullptr;
        gchar* dbg = nullptr;
        gst_message_parse_error(message, &err, &dbg);
        std::fprintf(stderr, "[spike] bus ERROR: %s (%s)\n",
                     err ? err->message : "?", dbg ? dbg : "");
        g_clear_error(&err);
        g_free(dbg);
        return GST_BUS_PASS;
    }
    case GST_MESSAGE_WARNING: {
        GError* err = nullptr;
        gchar* dbg = nullptr;
        gst_message_parse_warning(message, &err, &dbg);
        std::fprintf(stderr, "[spike] bus WARNING: %s (%s)\n",
                     err ? err->message : "?", dbg ? dbg : "");
        g_clear_error(&err);
        g_free(dbg);
        return GST_BUS_PASS;
    }
    case GST_MESSAGE_EOS:
        std::fprintf(stderr, "[spike] bus EOS\n");
        return GST_BUS_PASS;
    case GST_MESSAGE_STATE_CHANGED: {
        if (GST_MESSAGE_SRC(message) == GST_OBJECT(self->pipeline_)) {
            GstState oldState = GST_STATE_NULL, newState = GST_STATE_NULL;
            gst_message_parse_state_changed(message, &oldState, &newState,
                                            nullptr);
            std::fprintf(stderr, "[spike] pipeline state: %s -> %s\n",
                         gst_element_state_get_name(oldState),
                         gst_element_state_get_name(newState));
        }
        return GST_BUS_PASS;
    }
    default:
        break;
    }

    if (msgType != GST_MESSAGE_NEED_CONTEXT) {
        return GST_BUS_PASS;
    }

    const gchar* contextType = nullptr;
    gst_message_parse_context_type(message, &contextType);
    if (g_strcmp0(contextType, GST_GL_DISPLAY_CONTEXT_TYPE) == 0) {
        GstContext* ctx = gst_context_new(GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
        gst_context_set_gl_display(ctx, self->glDisplay_);
        gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(message)), ctx);
        gst_context_unref(ctx);
        return GST_BUS_DROP;
    }
    if (g_strcmp0(contextType, "gst.gl.app_context") == 0) {
        GstContext* ctx = gst_context_new("gst.gl.app_context", TRUE);
        GstStructure* s = gst_context_writable_structure(ctx);
        gst_structure_set(s, "context", GST_TYPE_GL_CONTEXT, self->glContext_,
                          nullptr);
        gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(message)), ctx);
        gst_context_unref(ctx);
        return GST_BUS_DROP;
    }
    return GST_BUS_PASS;
}

GstFlowReturn GesVideoItem::onNewSample(GstAppSink* sink, gpointer userData) {
    auto* self = static_cast<GesVideoItem*>(userData);
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_ERROR;
    self->handleSample(sample);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

void GesVideoItem::handleSample(GstSample* sample) {
    GstCaps* caps = gst_sample_get_caps(sample);
    if (!caps) {
        std::fprintf(stderr, "[spike] sample has no caps\n");
        return;
    }

    // The whole point: does the buffer carry GL memory (a texture Qt consumes
    // directly), a zero-copy DMABuf (GPU-resident but needs an EGLImage
    // import), or did something already copy it to system memory?
    GstCapsFeatures* features = gst_caps_get_features(caps, 0);
    const bool isGl =
        gst_caps_features_contains(features, "memory:GLMemory");
    const bool isDma =
        gst_caps_features_contains(features, "memory:DMABuf");
    const bool isSystem =
        gst_caps_features_contains(features, "memory:SystemMemory");

    ++frameCount_;
    if (frameCount_ == 1) {
        gchar* capsStr = gst_caps_to_string(caps);
        gpuResident_ = isGl || isDma;
        if (isGl) {
            framePath_ = QStringLiteral("GPU-resident GL texture: %1").arg(capsStr);
        } else if (isDma) {
            framePath_ = QStringLiteral("GPU-resident DMABuf: %1").arg(capsStr);
        } else if (isSystem) {
            framePath_ = QStringLiteral("CPU readback (system memory): %1").arg(capsStr);
        } else {
            framePath_ = QStringLiteral("unknown memory type: %1").arg(capsStr);
        }
        std::fprintf(stderr, "[spike] first frame: %s\n",
                     framePath_.toUtf8().constData());
        g_free(capsStr);
        emit gpuResidentChanged();
        emit framePathChanged();
    }
    emit frameCountChanged();

    if (isGl) {
        stageFrame(gst_sample_get_buffer(sample));
    }
}

bool GesVideoItem::stageFrame(GstBuffer* buffer) {
    // Extract a single RGBA GL texture id from the buffer. This runs on
    // GStreamer's streaming thread; it only reads a struct field and takes a
    // reference, so no GL context is needed here.
    GstMemory* mem = gst_buffer_peek_memory(buffer, 0);
    if (!mem || !gst_is_gl_memory(mem)) {
        std::fprintf(stderr, "[spike] buffer memory is not GL memory\n");
        return false;
    }
    auto* glMem = GST_GL_MEMORY_CAST(mem);
    const guint texId = gst_gl_memory_get_texture_id(glMem);
    const gint texW = gst_gl_memory_get_texture_width(glMem);
    const gint texH = gst_gl_memory_get_texture_height(glMem);
    if (!stageLogged_) {
        stageLogged_ = true;
        std::fprintf(stderr,
                     "[spike] stageFrame: GL id=%u %dx%d (mem count=%zu)\n",
                     texId, texW, texH,
                     static_cast<size_t>(gst_buffer_n_memory(buffer)));
    }
    if (texId == 0 || texW <= 0 || texH <= 0) {
        std::fprintf(stderr, "[spike] invalid GL texture (id=%u %dx%d)\n",
                     texId, texW, texH);
        return false;
    }

    PendingFrame frame;
    frame.buffer = gst_buffer_ref(buffer);
    frame.textureId = texId;
    frame.width = texW;
    frame.height = texH;

    {
        QMutexLocker lock(&frameMutex_);
        if (pending_.buffer) gst_buffer_unref(pending_.buffer);
        pending_ = frame;
    }

    // Mark the item dirty so the scene graph re-runs updatePaintNode. The item
    // update() (not window()->update()) is what schedules the paint-node
    // re-evaluation; it is safe to call from the GStreamer streaming thread.
    update();
    return true;
}

QSGNode* GesVideoItem::updatePaintNode(QSGNode* oldNode,
                                       UpdatePaintNodeData* data) {
    (void)data;

    auto* node = static_cast<QSGSimpleTextureNode*>(oldNode);
    if (!node) {
        node = new QSGSimpleTextureNode();
        // glcolorconvert renders into a texture whose row 0 is the top of the
        // image, matching Qt Quick's texture-coordinate origin, so no
        // coordinate transform is needed (NoTransform is the default).
        node->setFiltering(QSGTexture::Linear);
    }
    node_ = node;

    // All GL work (wrapping the GStreamer texture in a QRhiTexture, deleting
    // the previous one) happens here, on the render thread, where the scene
    // graph has already made the RHI/GL context current. This avoids making
    // the context current from the GStreamer streaming thread.
    PendingFrame frame;
    {
        QMutexLocker lock(&frameMutex_);
        frame = pending_;
        pending_ = PendingFrame{};
    }
    if (!frame.buffer) {
        return node;
    }

    QQuickWindow* win = window();
    QRhi* rhi = win ? win->rhi() : nullptr;
    if (!rhi) {
        std::fprintf(stderr, "[spike] no RHI in updatePaintNode\n");
        gst_buffer_unref(frame.buffer);
        return node;
    }

    // Wrap the externally-owned GL texture id (GStreamer keeps it alive via
    // frame.buffer) in a QRhiTexture, then in a QSGTexture the node samples.
    QRhiTexture* rhiTex =
        rhi->newTexture(QRhiTexture::RGBA8, QSize(frame.width, frame.height));
    if (!rhiTex) {
        std::fprintf(stderr, "[spike] newTexture failed\n");
        gst_buffer_unref(frame.buffer);
        return node;
    }
    if (!rhiTex->createFrom({ quint64(frame.textureId), 0 })) {
        std::fprintf(stderr, "[spike] QRhiTexture::createFrom failed (id=%u)\n",
                     frame.textureId);
        delete rhiTex;
        gst_buffer_unref(frame.buffer);
        return node;
    }

    QSGTexture* sgTex = win->createTextureFromRhiTexture(
        rhiTex, QQuickWindow::TextureHasAlphaChannel);
    if (!sgTex) {
        std::fprintf(stderr, "[spike] createTextureFromRhiTexture failed\n");
        delete rhiTex;
        gst_buffer_unref(frame.buffer);
        return node;
    }

    node->setTexture(sgTex);
    node->setRect(boundingRect());
    node->markDirty(QSGNode::DirtyMaterial);

    // Swap out the previous frame's resources. createTextureFromRhiTexture()
    // returns a QSGPlainTexture that owns the QRhiTexture, so deleting the
    // QSGTexture frees the QRhiTexture as well. The GstBuffer unref releases
    // the GL texture id back to GStreamer (the QRhiTexture wraps, never owns,
    // the native object).
    if (sgTexture_) delete sgTexture_;
    if (activeBuffer_) gst_buffer_unref(activeBuffer_);
    sgTexture_ = sgTex;
    activeBuffer_ = frame.buffer;

    ++rendered_;
    emit renderedChanged();
    if (rendered_ <= 3) {
        std::fprintf(stderr,
                     "[spike] committed textured node #%d (%ux%u, GL id=%u)\n",
                     rendered_, frame.width, frame.height, frame.textureId);
    }
    return node;
}

void GesVideoItem::releaseResources() {
    // Scene-graph invalidated or item destroyed: drop our references to the
    // committed texture. Called on the GUI thread; deleting the QSGTexture also
    // frees the QRhiTexture it owns.
    if (sgTexture_) {
        delete sgTexture_;
        sgTexture_ = nullptr;
    }
    if (activeBuffer_) {
        gst_buffer_unref(activeBuffer_);
        activeBuffer_ = nullptr;
    }
    node_ = nullptr;  // owned by the scene graph
    QMutexLocker lock(&frameMutex_);
    if (pending_.buffer) {
        gst_buffer_unref(pending_.buffer);
        pending_ = PendingFrame{};
    }
}
