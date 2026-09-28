// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Phase 0 spike: present a GES pipeline inside a Qt Quick scene.
//
// The question this answers is narrow and load-bearing: when GES produces a
// frame, can it reach the Qt Quick scene graph without an unconditional CPU
// readback? The item below runs a GES pipeline into an appsink, classifies the
// arriving buffer (GL memory / DMABuf / system memory), and when it is a single
// GL texture, wraps that texture id in a QRhiTexture and commits it to the
// scene graph as a QSGSimpleTextureNode.

#pragma once

#include <QQuickItem>
#include <QImage>
#include <QMutex>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/gl/gl.h>

class QOpenGLContext;
class QSGSimpleTextureNode;
class QSGTexture;

class GesVideoItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    // Path to the media file the spike timeline plays.
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
    // Frames observed so far (delivered to the appsink), for the readout.
    Q_PROPERTY(int frameCount READ frameCount NOTIFY frameCountChanged)
    // Frames actually committed to the scene graph as a textured node.
    Q_PROPERTY(int rendered READ rendered NOTIFY renderedChanged)
    // True when the most recent frame arrived as GL memory (no readback).
    Q_PROPERTY(bool gpuResident READ gpuResident NOTIFY gpuResidentChanged)
    // Human-readable description of the frame path actually observed.
    Q_PROPERTY(QString framePath READ framePath NOTIFY framePathChanged)

public:
    explicit GesVideoItem(QQuickItem* parent = nullptr);
    ~GesVideoItem() override;

    QString source() const { return source_; }
    void setSource(const QString& source);

    int frameCount() const { return frameCount_; }
    int rendered() const { return rendered_; }
    bool gpuResident() const { return gpuResident_; }
    QString framePath() const { return framePath_; }

signals:
    void sourceChanged();
    void frameCountChanged();
    void renderedChanged();
    void gpuResidentChanged();
    void framePathChanged();

protected:
    // QQuickItem hooks: start the pipeline once the item is on a window, and
    // commit each arriving GL frame to the scene graph.
    void componentComplete() override;
    QSGNode* updatePaintNode(QSGNode* oldNode,
                             UpdatePaintNodeData* data) override;
    void releaseResources() override;

private:
    static GstFlowReturn onNewSample(GstAppSink* sink, gpointer userData);
    static GstBusSyncReply onBusSync(GstBus* bus, GstMessage* message,
                                     gpointer userData);
    void handleSample(GstSample* sample);
    void startPipeline();
    void stopPipeline();

    // Step 1: a self-created EGL context (gst_gl_display_egl_new), used to
    // isolate the decoder variable from the context variable.
    bool createSelfContext();
    // Step 2: wrap Qt's RHI OpenGL context so GStreamer renders into the same
    // share group the scene graph draws from.
    bool wrapQtContext();
    // Build and start the probe pipeline once a context is available.
    void buildAndStartPipeline();
    // True when a (re)attempt should keep polling for the RHI context; aborts
    // after a bounded number of tries so the spike never spins forever.
    void deferStart();

    // A frame handed from the GStreamer streaming thread to the render thread.
    // The buffer reference keeps the GL texture (owned by its GstGLMemory)
    // alive until the render thread has finished sampling it.
    struct PendingFrame {
        GstBuffer* buffer = nullptr;
        guint textureId = 0;
        int width = 0;
        int height = 0;
    };

    // Streaming thread: extract a single RGBA GL texture from the buffer and
    // park it for the render thread. Returns false when the buffer is not a
    // single presentable GL texture.
    bool stageFrame(GstBuffer* buffer);

    QString source_;
    int frameCount_ = 0;
    int rendered_ = 0;
    bool gpuResident_ = false;
    QString framePath_ = QStringLiteral("not started");

    GstElement* pipeline_ = nullptr;
    GstElement* sink_ = nullptr;
    GstGLDisplay* glDisplay_ = nullptr;
    GstGLContext* glContext_ = nullptr;
    QOpenGLContext* qtContext_ = nullptr;  // kept alive while the pipeline runs
    int contextAttempts_ = 0;
    bool started_ = false;

    // Guards pending_ (streaming thread writes, render thread reads).
    QMutex frameMutex_;
    PendingFrame pending_;

    // Render-thread-only state for the committed texture. The node itself is
    // owned by the scene graph; the QSGTexture is owned here and swapped out
    // (deleted + GstBuffer unreffed) on every new frame. The QSGTexture owns
    // the QRhiTexture it wraps, which in turn wraps (never owns) the GL id.
    QSGSimpleTextureNode* node_ = nullptr;
    QSGTexture* sgTexture_ = nullptr;
    GstBuffer* activeBuffer_ = nullptr;
    bool stageLogged_ = false;
};
