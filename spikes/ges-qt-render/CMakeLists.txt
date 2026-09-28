cmake_minimum_required(VERSION 3.21)
project(ges_qt_render LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# GES ships no .pc file on Arch; link it explicitly alongside the GStreamer flags.
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-gl-1.0 gstreamer-gl-egl-1.0)

# GuiPrivate is only for the <rhi/qrhi.h> include path: on this distro QRhi
# headers are installed under the versioned private include dir, not the public
# QtGui dir. QRhi itself is public Qt API with limited compatibility guarantees.
find_package(Qt6 REQUIRED COMPONENTS Core Gui GuiPrivate Quick Qml)

qt_standard_project_setup()

qt_add_executable(ges_qt_render
    main.cpp
)

qt_add_qml_module(ges_qt_render
    URI GesSpike
    VERSION 1.0
    QML_FILES Main.qml
    SOURCES
        GesVideoItem.h
        GesVideoItem.cpp
)

target_link_libraries(ges_qt_render PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::GuiPrivate
    Qt6::Quick
    Qt6::Qml
    PkgConfig::GST
    ges-1.0
)

# The QML file is loaded from the source dir so the spike stays a single tree.
target_compile_definitions(ges_qt_render PRIVATE
    SPIKE_QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
