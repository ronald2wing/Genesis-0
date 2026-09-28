// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Phase 0 spike entry point. Runs a Qt Quick window whose scene contains a
// GES-fed video item, and exits after a fixed observation window so the spike
// can run unattended and report a verdict.

#include <cstdio>

// GStreamer/GES pull in glib, whose gdbusintrospection.h has a field named
// `signals` - a Qt keyword macro. Include the glib headers first so the field
// is parsed before Qt defines the macro.
#include <gst/gst.h>
#include <ges/ges.h>

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>
#include <QDebug>

int main(int argc, char* argv[]) {
    std::fprintf(stderr, "[spike] main entered\n");
    gst_init(&argc, &argv);
    ges_init();
    std::fprintf(stderr, "[spike] gst/ges initialised\n");

    // Phase 0 gate: force Qt Quick's RHI to OpenGL so the spike can wrap Qt's
    // own GL context and share it with GStreamer. Must run before any window.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    std::fprintf(stderr, "[spike] RHI backend forced to OpenGL\n");

    QGuiApplication app(argc, argv);

    // The media file the spike plays: first positional argument, or the
    // sample the engine-half spike already produced.
    const QString source = argc > 1
                               ? QString::fromLocal8Bit(argv[1])
                               : QStringLiteral("/tmp/opencode/ges-spike/a.webm");

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("spikeSource"),
                                             source);
    std::fprintf(stderr, "[spike] loading QML from %s\n", SPIKE_QML_DIR "/Main.qml");
    engine.load(QUrl::fromLocalFile(
        QStringLiteral(SPIKE_QML_DIR) + QStringLiteral("/Main.qml")));

    if (engine.rootObjects().isEmpty()) {
        std::fprintf(stderr, "[spike] QML failed to load\n");
        return 1;
    }
    std::fprintf(stderr, "[spike] QML loaded, %lld root objects\n",
                 static_cast<long long>(engine.rootObjects().size()));

    // The spike is a probe, not an app: observe for a few seconds, then quit.
    QTimer::singleShot(5000, &app, &QGuiApplication::quit);
    return app.exec();
}
