# SPDX-FileCopyrightText: 2026 Genesis-0 contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A standalone build for the hello-extension example. It is deliberately NOT
# part of the Genesis-0 CMake build: an Extension is built against the SDK
# header and installed into the host's Extension store, not linked into the app.
#
#   cmake -S . -B build -G Ninja
#   cmake --build build
#
# The shared library lands BESIDE extension.toml (the parser requires `entry` to
# be a bare file name next to the manifest), so the whole example directory is
# installable as-is afterwards.

cmake_minimum_required(VERSION 3.28)
project(hello_extension C)

# Path to the Genesis-0 `src/` directory, which holds extensions/ExtensionApi.h.
# Override with -DGENESIS_SRC=... when building the example out of tree.
set(GENESIS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../src" CACHE PATH
    "Path to the Genesis-0 src/ directory (for extensions/ExtensionApi.h)")

add_library(hello SHARED hello.c)

target_include_directories(hello PRIVATE "${GENESIS_SRC}")

# Put the shared library next to extension.toml instead of in a build subdir.
set_target_properties(hello PROPERTIES
    OUTPUT_NAME hello
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

if(MSVC)
    target_compile_options(hello PRIVATE /W4)
else()
    target_compile_options(hello PRIVATE -Wall -Wextra -Wpedantic)
endif()
