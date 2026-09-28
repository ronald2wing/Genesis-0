// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The Genesis-0 entry point: GStreamer/GES and Qt bootstrap, then the studio
// shell (AppShell) builds the controllers, wires the signals, registers the
// QML context and runs the event loop.

// GStreamer/GES pull in glib, whose gdbusintrospection.h has a field named
// `signals` - a Qt keyword macro. Include the glib headers first so the field
// is parsed before Qt defines the macro.
#include <gst/gst.h>
#include <ges/ges.h>

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QSGRendererInterface>

#include "app/AppShell.h"
#include "app/support/AppLog.h"

int main(int argc, char *argv[])
{
    // The monitor presents GStreamer GL frames by sharing the scene graph's
    // EGL context. GStreamer's GL elements otherwise pick their display from
    // the environment (Wayland on a Wayland session), and a context can only be
    // set on an element whose display type matches - a Wayland display cannot
    // share with Qt's EGL context, so the uploaded textures are invisible to
    // the RHI and the monitor shows garbled stripes. Pin the GL platform and
    // window to EGL before gst_init so the elements adopt the app's display.
    g_setenv("GST_GL_PLATFORM", "egl", TRUE);
    g_setenv("GST_GL_WINDOW", "egl", TRUE);

    gst_init(&argc, &argv);
    ges_init();

    // Force Qt Quick's RHI to OpenGL before any window exists, so the engine
    // can share the scene graph's own GL context.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);

    QGuiApplication app(argc, argv);

    // Dev-only flags, documented here because they are the only way to render
    // the shell without a display:
    //   --screenshot <path>        capture the window to <path> as a PNG, then
    //                              quit (dismisses the launch screen first).
    //   --exit-after <ms>          quit after <ms>; with --screenshot it is the
    //                              capture deadline, alone it is a clean auto-
    //                              quit smoke run.
    //   --screenshot-open <action> before capture, invoke Main.qml's
    //                              runAction(<action>) so a dialog (e.g.
    //                              "titles", "speech") can be captured too.
    // The first positional argument is the project document/folder (unchanged).
    QCommandLineParser parser;
    parser.setApplicationDescription("Genesis-0 studio shell");
    parser.addHelpOption();
    parser.addPositionalArgument(
            QStringLiteral("project"),
            QStringLiteral("Project document or folder to open (default: demo)."),
            QStringLiteral("[project]"));
    const QCommandLineOption screenshot(
            QStringLiteral("screenshot"),
            QStringLiteral("Capture the window to <path> as a PNG and quit."),
            QStringLiteral("path"));
    const QCommandLineOption exitAfter(
            QStringLiteral("exit-after"),
            QStringLiteral("Quit after <ms> (bounds the screenshot wait)."), QStringLiteral("ms"));
    const QCommandLineOption screenshotOpen(
            QStringLiteral("screenshot-open"),
            QStringLiteral("Invoke Main.qml runAction(<action>) before capture."),
            QStringLiteral("action"));
    const QCommandLineOption closeAfter(
            QStringLiteral("close-after"),
            QStringLiteral("Close the window after <ms> (the interactive-close path)."),
            QStringLiteral("ms"));
    const QCommandLineOption newProjectAfter(
            QStringLiteral("new-project-after"),
            QStringLiteral("Run the New Project path after <ms> (reload while playing)."),
            QStringLiteral("ms"));
    const QCommandLineOption logLevel(
            QStringLiteral("log-level"),
            QStringLiteral("Log verbosity: error, warn, info or debug (overrides GENESIS_LOG)."),
            QStringLiteral("level"));
    parser.addOption(screenshot);
    parser.addOption(exitAfter);
    parser.addOption(screenshotOpen);
    parser.addOption(closeAfter);
    parser.addOption(newProjectAfter);
    parser.addOption(logLevel);
    parser.process(app);

    // Log level, once at startup: an explicit --log-level wins, otherwise
    // GENESIS_LOG in the environment. Both route through the same name parser;
    // an unrecognised value is reported and the level becomes info.
    const QString flag_level = parser.value(logLevel);
    if (!flag_level.isEmpty()) {
        genesis::app::apply_log_level(flag_level.toStdString());
    } else {
        genesis::app::apply_environment_log_level();
    }

    const QStringList positional = parser.positionalArguments();

    // The shell owns everything past the bootstrap: the editor, the
    // controllers, the signal wiring, the QML context and the event loop.
    AppShell shell(app, positional.isEmpty() ? QString() : positional.first(),
                   parser.value(screenshot), parser.value(exitAfter).toInt(),
                   parser.value(screenshotOpen), parser.value(closeAfter).toInt(),
                   parser.value(newProjectAfter).toInt());
    return shell.run();
}
