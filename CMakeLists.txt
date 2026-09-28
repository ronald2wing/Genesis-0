cmake_minimum_required(VERSION 3.28)

# This project has no C++ modules, but CMake 4.4 + GCC 16 default to scanning
# for them, which injects module flags (-fmodules-ts, -fmodule-mapper,
# -fdeps-format=…) into every translation unit. Those flags land in the exported
# compile_commands.json and clang-tidy's frontend rejects them as unknown, so
# the manual `tidy` target drowns in spurious "compiler error" noise. Turning
# the scan off keeps the compile DB clang-tidy-clean.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# The app version the notify-only updater compares against: bump here with a
# release. Kept in one place; Version.h falls back to this same default when
# GENESIS_APP_VERSION is not defined (e.g. a controller test target).
project(genesis0 VERSION 0.1.0 LANGUAGES CXX)

# The native-extension fixture is a shared library written in C (the ABI header
# is C99), so the C language is enabled alongside CXX for that one MODULE
# target.
enable_language(C)

# AI subsystem gates. All default OFF, so a default checkout never fetches a
# model runtime and the build stays dependency-free. GENESIS_AI is the master
# switch every AI runtime sits behind; GENESIS_AI_CAPTIONS turns on the
# whisper.cpp transcription runtime, GENESIS_AI_VISION the ONNX Runtime
# segmentation/enhance/brush runtime, and GENESIS_AI_TTS the sherpa-onnx
# speech runtime (each implies the master switch). The genesis_ai library
# always builds - its providers report "disabled" when their runtime is not
# compiled in, which is how the app links one target and asks it what it can do
# at runtime.
option(GENESIS_AI "build the AI subsystem (models + providers)" OFF)
option(GENESIS_AI_CAPTIONS "build the whisper.cpp caption runtime (implies GENESIS_AI)" OFF)
option(GENESIS_AI_VISION "build the ONNX Runtime segmentation runtime (implies GENESIS_AI)" OFF)
option(GENESIS_AI_TTS "build the sherpa-onnx Kokoro speech runtime (implies GENESIS_AI)" OFF)
option(GENESIS_AI_GPU "build whisper.cpp with CUDA acceleration (implies GENESIS_AI_CAPTIONS)" OFF)
if(GENESIS_AI_GPU)
    set(GENESIS_AI_CAPTIONS ON)
endif()
if(GENESIS_AI_CAPTIONS)
    set(GENESIS_AI ON)
endif()
if(GENESIS_AI_VISION)
    set(GENESIS_AI ON)
endif()
if(GENESIS_AI_TTS)
    set(GENESIS_AI ON)
endif()

# Self-update core gate. Default OFF, so a default checkout stays
# dependency-free: the updater sources still compile into genesis_host but
# report "disabled" (the Ed25519 verifier resolves to Unavailable, and install
# refuses before downloading anything). GENESIS_UPDATER=ON finds OpenSSL for
# the EVP Ed25519 check; if the lib is absent the build still succeeds, with
# the updater left disabled and a warning. GENESIS_UPDATER_ENABLED is the
# compile-time flag the sources key off - set only when the option is ON *and*
# OpenSSL was found.
option(GENESIS_UPDATER "build the self-update core with signed-manifest Ed25519 verification (requires OpenSSL)" OFF)
set(GENESIS_UPDATER_ENABLED OFF)
if(GENESIS_UPDATER)
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND)
        set(GENESIS_UPDATER_ENABLED ON)
    else()
        message(WARNING
            "GENESIS_UPDATER=ON but OpenSSL was not found. The updater still "
            "builds but reports 'disabled' - no signature is verified and no "
            "install succeeds.")
    endif()
endif()

# gRPC server gate. Default OFF: gRPC is a heavy dependency (Abseil, protobuf,
# BoringSSL, re2, c-ares, zlib - all built from gRPC's vendored submodules when
# no installed config satisfies it), so a default checkout never fetches it and
# the build stays dependency-free. GENESIS_GRPC=ON enables the `serve --grpc`
# transport (the same JSON-RPC codec the stdio/socket verbs use) plus its test
# suite. GENESIS_GRPC_ENABLED is set only when the option is ON *and* gRPC (and
# protobuf) resolve; the CLI keys its `--grpc` surface off this flag.
option(GENESIS_GRPC "build the gRPC server transport (requires gRPC + protobuf)" OFF)

# Small third-party libraries are fetched and version-pinned, so a fresh
# checkout needs only Qt6 and GStreamer/GES installed - not a distro package
# per helper library. FIND_PACKAGE_ARGS makes CMake prefer an installed
# package when it satisfies the minimum, and download otherwise; either way
# the version floor is ours, not the distro's.
include(FetchContent)

FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
    FIND_PACKAGE_ARGS 3.11 NAMES nlohmann_json
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(nlohmann_json)

# toml++ parses effect.toml manifests. The Arch package ships a CMake config
# (tomlplusplus::tomlplusplus), so find_package prefers it; a non-Arch checkout
# falls back to the same pinned tag, with the version floor still ours.
FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
    FIND_PACKAGE_ARGS 3.4 NAMES tomlplusplus
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(tomlplusplus)

# CLI11 parses the host CLI's verbs. BSD-3, header-only; the Arch package ships
# a CMake config (CLI11::CLI11), so find_package prefers it, and a non-Arch
# checkout falls back to the same pinned tag with the version floor still ours.
FetchContent_Declare(CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        v2.7.2
    FIND_PACKAGE_ARGS 2.7 NAMES CLI11
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(CLI11)

# Collect the transitive INTERFACE_INCLUDE_DIRECTORIES of `targets` (the
# third-party header roots a gRPC consumer sees) so they can be re-exported as
# SYSTEM. Recurse through INTERFACE_LINK_LIBRARIES, skipping generator
# expressions - a $<LINK_ONLY:...> entry is a PRIVATE link that contributes no
# include dirs anyway, and any other genex names no inspectable target.
function(genesis_collect_include_dirs out_var)
    set(_work ${ARGN})
    set(_seen)
    set(_dirs)
    while(_work)
        list(POP_FRONT _work _tgt)
        if(NOT TARGET ${_tgt})
            continue()
        endif()
        list(FIND _seen ${_tgt} _hit)
        if(NOT _hit EQUAL -1)
            continue()
        endif()
        list(APPEND _seen ${_tgt})
        get_target_property(_inc ${_tgt} INTERFACE_INCLUDE_DIRECTORIES)
        if(_inc)
            list(APPEND _dirs ${_inc})
        endif()
        get_target_property(_links ${_tgt} INTERFACE_LINK_LIBRARIES)
        foreach(_l IN LISTS _links)
            if(_l MATCHES "\\$<")
                continue()
            endif()
            if(TARGET ${_l})
                list(APPEND _work ${_l})
            endif()
        endforeach()
    endwhile()
    list(REMOVE_DUPLICATES _dirs)
    set(${out_var} ${_dirs} PARENT_SCOPE)
endfunction()

# gRPC documents `find_package(gRPC CONFIG)` first, with a FetchContent
# fallback pinned to a tag. The fallback is heavy - tens of minutes cold - so
# it only runs behind GENESIS_GRPC. GENESIS_GRPC_ENABLED records whether gRPC
# actually resolved, and is the flag the CLI and test registration key off.
set(GENESIS_GRPC_ENABLED OFF)
if(GENESIS_GRPC)
    find_package(gRPC CONFIG QUIET)
    if(gRPC_FOUND)
        # An installed gRPC config pulls protobuf via find_dependency; ensure
        # the protoc executable target is present for codegen below.
        find_package(Protobuf CONFIG QUIET)
    else()
        # gRPC builds its own tree as C++17; pin the standard for this fetch so
        # the repo's C++23 (set below) never leaks into gRPC's targets.
        set(CMAKE_CXX_STANDARD 17)
        set(gRPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(gRPC_INSTALL OFF CACHE BOOL "" FORCE)
        # gRPC's vendored protobuf links libprotoc against Abseil, but Abseil is
        # built here as a gRPC module with ABSL_STANDALONE unset, so its targets
        # never enter an export set. protobuf's own install(EXPORT) then fails at
        # generate time trying to export libprotoc (and utf8_range's export does
        # the same through absl::strings). Neither install rule matters to a
        # fetched, in-tree build, so turn both off.
        set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
        set(utf8_range_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        # The C++ plugin (protoc-gen-grpc) is all this project consumes; the
        # other language plugins stay off. Dependency providers keep their
        # "module" default so the build is self-contained.
        set(gRPC_BUILD_GRPC_CPP_PLUGIN ON CACHE BOOL "" FORCE)
        set(gRPC_BUILD_GRPC_CSHARP_PLUGIN OFF CACHE BOOL "" FORCE)
        set(gRPC_BUILD_GRPC_NODE_PLUGIN OFF CACHE BOOL "" FORCE)
        set(gRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN OFF CACHE BOOL "" FORCE)
        set(gRPC_BUILD_GRPC_PHP_PLUGIN OFF CACHE BOOL "" FORCE)
        set(gRPC_BUILD_GRPC_PYTHON_PLUGIN OFF CACHE BOOL "" FORCE)
        set(gRPC_BUILD_GRPC_RUBY_PLUGIN OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(gRPC
            GIT_REPOSITORY https://github.com/grpc/grpc.git
            GIT_TAG        v1.82.0
            # gRPC's module providers add_subdirectory() their dependencies out
            # of third_party/, which are git submodules; list the six a C++
            # build needs. The xDS proto dirs are fetched by
            # gRPC_DOWNLOAD_ARCHIVES at configure time instead.
            GIT_SUBMODULES
                third_party/abseil-cpp
                third_party/boringssl-with-bazel
                third_party/cares/cares
                third_party/protobuf
                third_party/re2
                third_party/zlib
            EXCLUDE_FROM_ALL
        )
        FetchContent_MakeAvailable(gRPC)
        unset(CMAKE_CXX_STANDARD)
    endif()

    # gRPC only names its targets gRPC::grpc++ / gRPC::grpc_cpp_plugin when
    # installed; a fetched build names them grpc++ / grpc_cpp_plugin. Alias the
    # fetched names so the rest of this file refers to one spelling.
    if(NOT TARGET gRPC::grpc++)
        add_library(gRPC::grpc++ ALIAS grpc++)
    endif()
    if(NOT TARGET gRPC::grpc_cpp_plugin)
        add_executable(gRPC::grpc_cpp_plugin ALIAS grpc_cpp_plugin)
    endif()

    set(GENESIS_GRPC_ENABLED ON)

    # Codegen: protoc + grpc_cpp_plugin over proto/genesis.proto, into the
    # binary dir. The generated sources compile as a small static library that
    # the CLI (and the gRPC suite) link.
    set(GENESIS_GRPC_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto)
    file(MAKE_DIRECTORY ${GENESIS_GRPC_GEN_DIR})
    set(GENESIS_GRPC_GEN
        ${GENESIS_GRPC_GEN_DIR}/genesis.pb.cc
        ${GENESIS_GRPC_GEN_DIR}/genesis.pb.h
        ${GENESIS_GRPC_GEN_DIR}/genesis.grpc.pb.cc
        ${GENESIS_GRPC_GEN_DIR}/genesis.grpc.pb.h)
    add_custom_command(
        OUTPUT ${GENESIS_GRPC_GEN}
        COMMAND protobuf::protoc
            --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
            --cpp_out=${GENESIS_GRPC_GEN_DIR}
            --grpc_out=${GENESIS_GRPC_GEN_DIR}
            --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
            ${CMAKE_CURRENT_SOURCE_DIR}/proto/genesis.proto
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/proto/genesis.proto
        VERBATIM)
    add_library(genesis_grpc_proto STATIC
        ${GENESIS_GRPC_GEN_DIR}/genesis.pb.cc
        ${GENESIS_GRPC_GEN_DIR}/genesis.grpc.pb.cc)
    # Generated code is protoc output, not ours: expose its headers as SYSTEM
    # so -Werror never fires on them.
    target_include_directories(genesis_grpc_proto SYSTEM PUBLIC ${GENESIS_GRPC_GEN_DIR})
    # Link the runtime PRIVATE so the static library's own generated sources
    # compile against it, and CMake forwards the deps to consumers link-only
    # ($<LINK_ONLY:>) - the third-party include dirs never reach a consumer as
    # ordinary -I paths. genesis_warnings compiles consumers with -Wpedantic
    # -Werror, and Abseil's int128.h uses __int128 (a GNU extension -Wpedantic
    # rejects), so the headers are re-exported as SYSTEM below instead.
    target_link_libraries(genesis_grpc_proto PRIVATE gRPC::grpc++ protobuf::libprotobuf)
    genesis_collect_include_dirs(GENESIS_GRPC_SYSTEM_INCLUDES gRPC::grpc++ protobuf::libprotobuf)
    target_include_directories(genesis_grpc_proto SYSTEM INTERFACE ${GENESIS_GRPC_SYSTEM_INCLUDES})
endif()

# Standard C++23 only; the repo forbids compiler extensions (R11).
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# clang-tidy (and IDE tooling) key off compile_commands.json: export it so the
# manual `tidy` target and editors can resolve each translation unit's flags.
# The file lands in the build dir and is already git-ignored.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Boost.Multiprecision (header-only) supplies the exact fixed-width integers
# behind Rational arithmetic. Boost is a system dependency like Qt6, so it is
# found rather than fetched; Boost::boost is the header-only interface target.
find_package(Boost REQUIRED)

# Keep every test binary in one place so ctest finds them.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

enable_testing()

# Engine-free host core: no Qt, no GStreamer. The source list is explicit to
# match the other targets; add a file here when a new host translation unit is
# introduced rather than letting a glob silently pick it up.
set(GENESIS_HOST_SOURCES
    src/core/Animation.cpp
    src/core/ClipTimeline.cpp
    src/core/Frame.cpp
    src/core/ShaderPass.cpp
    src/core/SpeedCurve.cpp
    src/ai/ModelManifest.cpp
    src/ai/ModelStore.cpp
    src/ai/Sha256.cpp
    src/ai/vision/MaskStore.cpp
    src/project/Document.cpp
    src/project/document/Json.cpp
    src/project/document/Migrate.cpp
    src/project/document/Read.cpp
    src/project/document/Settle.cpp
    src/project/document/Write.cpp
    src/project/Editor.cpp
    src/project/IdMinter.cpp
    src/project/Undo.cpp
    src/project/command/Codec.cpp
    src/project/commands/Apply.cpp
    src/project/commands/Audio.cpp
    src/project/commands/ClipPlacement.cpp
    src/project/commands/ClipReshape.cpp
    src/project/commands/Media.cpp
    src/project/commands/Properties.cpp
    src/project/commands/Timelines.cpp
    src/project/commands/Tracks.cpp
    src/render/Catalogue.cpp
    src/render/EffectFallback.cpp
    src/render/ExportClip.cpp
    src/render/ExportPresets.cpp
    src/render/ExportSpec.cpp
    src/render/Flatten.cpp
    src/render/LutLoader.cpp
    src/render/MediaCache.cpp
    src/render/Peaks.cpp
    src/render/Presets.cpp
    src/render/ProxyCache.cpp
    src/render/Resolve.cpp
    src/render/ReverseCache.cpp
    src/render/TitleRequests.cpp
    src/render/Titles.cpp
    src/render/TitleTemplates.cpp
    src/render/Transitions.cpp
    src/effects/Pack.cpp
    src/effects/PackLoader.cpp
    src/effects/PackSource.cpp
    src/effects/Resolve.cpp
     src/extensions/Install.cpp
     src/extensions/Trust.cpp
     src/extensions/Consent.cpp
     src/extensions/DisabledList.cpp
     src/extensions/Origins.cpp
     src/extensions/RemovedList.cpp
     src/extensions/Seed.cpp
     src/extensions/Sources.cpp
     src/extensions/Config.cpp
     src/extensions/Catalog.cpp
     src/extensions/Subprocess.cpp
     src/extensions/SourceInstall.cpp
     src/extensions/NativeManifest.cpp
    src/extensions/NativeExtension.cpp
    src/extensions/ExtensionHost.cpp
    src/extensions/Manager.cpp
    src/jobs/Job.cpp
    src/jobs/Scheduler.cpp
    src/api/Api.cpp
    src/api/Codec.cpp
    src/api/Requests.cpp
    src/workspace/AssetPaths.cpp
    src/workspace/Logs.cpp
    src/workspace/Projects.cpp
    src/workspace/Templates.cpp
    src/workspace/update/Manifest.cpp
    src/workspace/update/Signature.cpp
    src/workspace/update/Updater.cpp
)

# Strict warnings, treated as errors (GCC/Clang).
set(GENESIS_WARNINGS -Wall -Wextra -Wpedantic -Werror)

# Applies the strict-warnings flag set to a target, but only for GCC/Clang.
function(genesis_warnings target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE ${GENESIS_WARNINGS})
    endif()
endfunction()

# Registers a test suite end to end: builds the executable (named <name>_tests
# to match the repo's convention), links it, applies strict warnings, and adds
# the ctest entry (named <name>). WORKING_DIRECTORY sets the test's cwd.
function(genesis_add_test)
    set(oneValueArgs NAME WORKING_DIRECTORY)
    set(multiValueArgs SOURCES LIBRARIES INCLUDE_DIRECTORIES COMPILE_OPTIONS)
    cmake_parse_arguments(GAT "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    add_executable(${GAT_NAME}_tests ${GAT_SOURCES})
    if(GAT_INCLUDE_DIRECTORIES)
        target_include_directories(${GAT_NAME}_tests PRIVATE ${GAT_INCLUDE_DIRECTORIES})
    endif()
    if(GAT_LIBRARIES)
        target_link_libraries(${GAT_NAME}_tests PRIVATE ${GAT_LIBRARIES})
    endif()
    if(GAT_COMPILE_OPTIONS)
        target_compile_options(${GAT_NAME}_tests PRIVATE ${GAT_COMPILE_OPTIONS})
    endif()
    genesis_warnings(${GAT_NAME}_tests)
    add_test(NAME ${GAT_NAME} COMMAND ${GAT_NAME}_tests)
    if(GAT_WORKING_DIRECTORY)
        set_tests_properties(${GAT_NAME} PROPERTIES WORKING_DIRECTORY ${GAT_WORKING_DIRECTORY})
    endif()
endfunction()

add_library(genesis_host STATIC ${GENESIS_HOST_SOURCES})
target_include_directories(genesis_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(genesis_host PUBLIC nlohmann_json::nlohmann_json tomlplusplus::tomlplusplus Boost::boost)
# dlopen/dlsym for the native-extension loader (NativeExtension.cpp).
target_link_libraries(genesis_host PUBLIC ${CMAKE_DL_LIBS})
# The compile-time asset location AssetPaths::asset_root() falls back to after
# the GENESIS_ASSET_DIR environment variable; it points at the source tree's
# assets/ so a developer build finds titles and LUTs without an install step.
# GENESIS_BUILTIN_EXTENSIONS_DIR is the builtin extension source Seed.cpp's
# builtin_root() checks first (the source tree's extensions/builtin/), so a
# developer build seeds builtins without an install step too.
target_compile_definitions(genesis_host PRIVATE
    GENESIS_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
    GENESIS_BUILTIN_EXTENSIONS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/extensions/builtin"
)
genesis_warnings(genesis_host)

# The updater's Ed25519 verifier keys off GENESIS_UPDATER_ENABLED; without it
# Signature.cpp reports "disabled". OpenSSL::Crypto is linked PRIVATE - the host
# library itself stays crypto-free in its public interface, and the static
# library's LINK_ONLY propagation carries -lcrypto to the consumers that link
# genesis_host when (and only when) the updater is enabled.
if(GENESIS_UPDATER_ENABLED)
    target_compile_definitions(genesis_host PRIVATE GENESIS_UPDATER_ENABLED)
    target_link_libraries(genesis_host PRIVATE OpenSSL::Crypto)
endif()

# whisper.cpp transcription runtime: fetched and version-pinned only when the
# caption provider is enabled. Fetched as a subproject it is not "standalone",
# so whisper's own tests/examples/server already default OFF; the cache pins
# below hold them off explicitly and quiet its third-party warning flags. Built
# static (BUILD_SHARED_LIBS OFF) so genesis_ai links a .a and ships no runtime
# dependency.
#
# GENESIS_AI_GPU=ON asks whisper to build its CUDA kernels: GGML_CUDA is turned
# on only when the CUDA toolkit is actually present, otherwise the build warns
# and whisper stays on CPU (the caption provider still reports "enabled", just
# without a GPU). GENESIS_AI_GPU_ENABLED is the compile-time flag the provider
# reads to report whether it runs on the GPU.
set(GENESIS_AI_GPU_ENABLED OFF)
if(GENESIS_AI_GPU)
    find_package(CUDAToolkit QUIET)
    if(CUDAToolkit_FOUND)
        set(GGML_CUDA ON CACHE BOOL "" FORCE)
        set(GENESIS_AI_GPU_ENABLED ON)
    else()
        message(WARNING
            "GENESIS_AI_GPU=ON but the CUDA toolkit was not found. whisper.cpp "
            "builds without GPU acceleration and transcribes on CPU.")
    endif()
endif()
if(GENESIS_AI_CAPTIONS)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    set(WHISPER_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(whisper
        GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
        GIT_TAG        v1.9.4
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(whisper)
endif()

# sherpa-onnx Kokoro TTS runtime: fetched and version-pinned only when the
# speech provider is enabled. It pulls onnxruntime in as a subproject (a heavy,
# pinned dependency), and its C API target carries the include root at the
# sherpa-onnx top level so `#include "sherpa-onnx/c-api/c-api.h"` resolves by
# linking. Built static (BUILD_SHARED_LIBS OFF) so genesis_ai links a .a and
# ships no runtime dependency.
if(GENESIS_AI_TTS)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(SHERPA_ONNX_ENABLE_C_API ON CACHE BOOL "" FORCE)
    set(SHERPA_ONNX_ENABLE_TTS ON CACHE BOOL "" FORCE)
    # A TTS-only consumer needs none of these: the websocket server/client,
    # speaker diarization and portaudio exist for the demo binaries and ASR
    # tooling, and each drags in extra subprojects (websocketpp, asio,
    # hclust-cpp). Turning them off keeps the fetched graph to the Kokoro TTS
    # path (onnxruntime, espeak-ng, piper-phonemize).
    set(SHERPA_ONNX_ENABLE_WEBSOCKET OFF CACHE BOOL "" FORCE)
    set(SHERPA_ONNX_ENABLE_SPEAKER_DIARIZATION OFF CACHE BOOL "" FORCE)
    set(SHERPA_ONNX_ENABLE_PORTAUDIO OFF CACHE BOOL "" FORCE)
    # sherpa-onnx vendors nlohmann/json (cmake/json.cmake) and re-adds it with
    # the default target name, colliding with the json target this project
    # already resolved above (nlohmann_json::nlohmann_json). Give the vendored
    # copy a distinct name; sherpa-onnx only consumes the headers, so the name
    # is irrelevant to it. Scoped (non-cache) so it never leaks past this gate.
    set(NLOHMANN_JSON_TARGET_NAME nlohmann_json_sherpa)
    FetchContent_Declare(sherpa_onnx
        GIT_REPOSITORY https://github.com/k2-fsa/sherpa-onnx.git
        GIT_TAG        040afe360a38e25daaa325ce8889abf93ea02609
        # The pin still carries Supertonic's four u8"\u20XX" string literals
        # (offline-tts-supertonic-unicode-processor.cc): under C++20+ a u8
        # literal is const char8_t[N], which cannot convert to the const char*
        # the surrounding std::pair wants, and GCC 16 rejects it. Drop the u8
        # prefix - the universal-character-name escapes still encode UTF-8 bytes
        # in the narrow literal, so the output is unchanged. No newer upstream
        # tag carries a fix (v1.13.8 predates the pin, and master is unchanged);
        # drop this PATCH_COMMAND once the pin moves past the offending literals.
        PATCH_COMMAND patch -p1 -i ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/sherpa-onnx-supertonic-u8.patch
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(sherpa_onnx)
endif()

# ONNX Runtime segmentation runtime: found from the system first, and fetched
# as a pinned prebuilt only when no install satisfies the request. ORT cannot
# be built from source in a sane configure step, so the fallback is the pinned
# Linux x64 release tarball rather than a git checkout; it unpacks to
# include/ + lib/libonnxruntime.so (SONAME libonnxruntime.so.1), so the caller
# must make that .so.1 discoverable at run time (rpath or ldconfig).
if(GENESIS_AI_VISION)
    find_package(onnxruntime CONFIG QUIET)
    if(NOT onnxruntime_FOUND)
        FetchContent_Declare(onnxruntime_prebuilt
            URL https://github.com/microsoft/onnxruntime/releases/download/v1.29.0/onnxruntime-linux-x64-1.29.0.tgz
            URL_HASH SHA256=c3fddc4f139a045b0c4902c57410f0694f1c2fdf9b6939fbe38b1aeae7cd14ba
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
            EXCLUDE_FROM_ALL
        )
        FetchContent_MakeAvailable(onnxruntime_prebuilt)
        add_library(onnxruntime::onnxruntime SHARED IMPORTED)
        set_target_properties(onnxruntime::onnxruntime PROPERTIES
            IMPORTED_LOCATION ${onnxruntime_prebuilt_SOURCE_DIR}/lib/libonnxruntime.so
            INTERFACE_INCLUDE_DIRECTORIES ${onnxruntime_prebuilt_SOURCE_DIR}/include
        )
    endif()
endif()

# The AI library: engine-free and Qt-free (R5/R6), it links the host core for
# the model store and, when a runtime is ON, that runtime. It is always built
# so the app links one target and queries runtime capability rather than
# compiling differently. Each provider source guards its runtime behind a
# GENESIS_AI_*_ENABLED macro; without it the provider reports "disabled".
add_library(genesis_ai STATIC
    src/ai/vision/ExecutionProvider.cpp
    src/ai/captions/WhisperProvider.cpp
    src/ai/vision/SegmentationProvider.cpp
    src/ai/vision/EnhanceProvider.cpp
    src/ai/vision/BrushProvider.cpp
    src/ai/vision/MaskDriver.cpp
    src/ai/tts/SherpaTtsProvider.cpp
)
target_include_directories(genesis_ai PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(genesis_ai PUBLIC genesis_host)
genesis_warnings(genesis_ai)
if(GENESIS_AI_CAPTIONS)
    target_compile_definitions(genesis_ai PRIVATE GENESIS_AI_CAPTIONS_ENABLED)
    target_link_libraries(genesis_ai PRIVATE whisper)
    # whisper.h pulls in ggml.h/ggml-cpu.h; mark both roots SYSTEM so -Werror
    # does not fire on the third-party headers.
    target_include_directories(genesis_ai SYSTEM PRIVATE
        ${whisper_SOURCE_DIR}/include
        ${whisper_SOURCE_DIR}/ggml/include)
endif()
# GENESIS_AI_GPU_ENABLED is the compile-time flag the caption provider reads
# (WhisperProvider::gpu()) and the AI status indicator reports. It is set only
# when the option was ON *and* the CUDA toolkit was found above, so a GPU build
# without CUDA stays on CPU and reports it.
if(GENESIS_AI_GPU_ENABLED)
    target_compile_definitions(genesis_ai PRIVATE GENESIS_AI_GPU_ENABLED)
endif()
if(GENESIS_AI_VISION)
    target_compile_definitions(genesis_ai PRIVATE GENESIS_AI_VISION_ENABLED)
    target_link_libraries(genesis_ai PRIVATE onnxruntime::onnxruntime)
endif()
if(GENESIS_AI_TTS)
    target_compile_definitions(genesis_ai PRIVATE GENESIS_AI_TTS_ENABLED)
    target_link_libraries(genesis_ai PRIVATE sherpa-onnx-c-api)
    # sherpa-onnx's c-api.h is reached as "sherpa-onnx/c-api/c-api.h", so the
    # include root is the sherpa-onnx source dir; mark it SYSTEM so -Werror
    # does not fire on the third-party headers.
    target_include_directories(genesis_ai SYSTEM PRIVATE
        ${sherpa_onnx_SOURCE_DIR})
endif()

# GES engine adapter: the only target that sees GStreamer/GES. The host
# library stays engine-free (R5/R6) - GES types never cross into genesis_host.
# GES ships no .pc file on Arch, so it is linked raw (-lges-1.0) against the
# GStreamer flags; its headers live under the gstreamer-1.0 include dir.
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
    gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-pbutils-1.0
    gstreamer-gl-1.0 gstreamer-controller-1.0)

# glshader uploads vec2/vec3/vec4 uniforms only through graphene's boxed
# GValues (gstglfiltershader `_set_uniform`); it is already a transitive
# dependency of gstreamer-gl, declared here so the adapter links what it uses.
pkg_check_modules(GRAPHENE REQUIRED IMPORTED_TARGET graphene-gobject-1.0)

add_library(genesis_engine_ges STATIC
    src/adapters/engine/ges/effects/ClipEffects.cpp
    src/adapters/engine/ges/effects/LutGrade.cpp
    src/adapters/engine/ges/session/GesBuilder.cpp
    src/adapters/engine/ges/session/GesSession.cpp
    src/adapters/engine/ges/Hardware.cpp
    src/adapters/engine/ges/Encoder.cpp
    src/adapters/engine/ges/media/Probe.cpp
    src/adapters/engine/ges/media/Proxy.cpp
    src/adapters/engine/ges/media/AudioFrames.cpp
    src/adapters/engine/ges/media/VideoFrames.cpp
    src/adapters/engine/ges/effects/RevealEffect.cpp
    src/adapters/engine/ges/effects/MaskEffect.cpp
    src/adapters/engine/ges/effects/ShaderChain.cpp
    src/adapters/engine/ges/Export.cpp
    src/adapters/engine/ges/media/Filmstrip.cpp
    src/adapters/engine/ges/scopes/Scopes.cpp
    src/adapters/engine/ges/media/Reverse.cpp
    src/adapters/engine/ges/effects/TextureMix.cpp
)
target_include_directories(genesis_engine_ges PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(genesis_engine_ges PUBLIC PkgConfig::GST PkgConfig::GRAPHENE ges-1.0 genesis_host)
genesis_warnings(genesis_engine_ges)

# The host-side CLI: inspect/validate/flatten over a document, read a Pack
# manifest, list the command verbs. The `render` verb reaches the engine, so
# the CLI links genesis_engine_ges (transitively the host core and GES); every
# other verb stays host-only. Qt-free. With GENESIS_GRPC the `serve --grpc`
# transport joins, linking the generated proto stub and the gRPC runtime.
set(GENESIS_CLI_SOURCES
    src/cli/main.cpp
    src/cli/Commands.cpp
    src/cli/JsonRpc.cpp
    src/cli/SocketServe.cpp
)
if(GENESIS_GRPC_ENABLED)
    list(APPEND GENESIS_CLI_SOURCES src/cli/GrpcServe.cpp)
endif()
add_executable(genesis_cli ${GENESIS_CLI_SOURCES})
set_target_properties(genesis_cli PROPERTIES OUTPUT_NAME "genesis-cli")
target_link_libraries(genesis_cli PRIVATE genesis_engine_ges genesis_host CLI11::CLI11)
if(GENESIS_GRPC_ENABLED)
    # GENESIS_GRPC_ENABLED guards the `--grpc` surface in main.cpp; the stub
    # brings the generated headers (SYSTEM) and the gRPC runtime transitively.
    target_compile_definitions(genesis_cli PRIVATE GENESIS_GRPC_ENABLED)
    target_link_libraries(genesis_cli PRIVATE genesis_grpc_proto)
endif()
genesis_warnings(genesis_cli)

# Ship the builtin extension source beside the CLI binary so builtin_root()'s
# beside-the-exe fallback finds it in a packaged (non-source-tree) run.
add_custom_command(TARGET genesis_cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/extensions/builtin
        $<TARGET_FILE_DIR:genesis_cli>/extensions/builtin
    VERBATIM)

# Host-only suites: no adapter, no Qt - they link genesis_host. The suites
# that resolve the builtin packs (pack_source, catalogue, reveal_pack,
# pack_batch) walk up from cwd to find extensions/builtin/genesis.packs/packs,
# so they run with the source dir as cwd.
genesis_add_test(NAME core SOURCES tests/CoreTests.cpp LIBRARIES genesis_host)
genesis_add_test(NAME project SOURCES tests/project/ProjectTests.cpp LIBRARIES genesis_host)
genesis_add_test(NAME document SOURCES tests/project/DocumentTests.cpp LIBRARIES genesis_host)
# The command codec suite: every Command to and from its tagged-`op` JSON,
# with the strict-decode and three-way optional semantics pinned.
genesis_add_test(NAME command_codec SOURCES tests/project/CommandCodecTests.cpp LIBRARIES genesis_host)
genesis_add_test(NAME clip_timeline SOURCES tests/ClipTimelineTests.cpp LIBRARIES genesis_host)
genesis_add_test(NAME flatten SOURCES tests/render/FlattenTests.cpp LIBRARIES genesis_host)
genesis_add_test(NAME resolve SOURCES tests/render/ResolveTests.cpp LIBRARIES genesis_host)
genesis_add_test(NAME peaks SOURCES tests/render/PeaksTests.cpp LIBRARIES genesis_host)
genesis_add_test(NAME pack SOURCES tests/effects/PackTests.cpp LIBRARIES genesis_host)
genesis_add_test(NAME pack_loader SOURCES tests/effects/PackLoaderTests.cpp LIBRARIES genesis_host)
genesis_add_test(NAME resolve_effects SOURCES tests/effects/ResolveTests.cpp LIBRARIES genesis_host)
genesis_add_test(NAME effect_fallback SOURCES tests/render/EffectFallbackTests.cpp LIBRARIES genesis_host)
 genesis_add_test(NAME install SOURCES tests/extensions/InstallTests.cpp LIBRARIES genesis_host)
# The install-source records' hermetic suite: type/url round-trip, the atomic
# JSON round-trip, and the corrupt-file-as-empty reading.
genesis_add_test(NAME sources SOURCES tests/extensions/SourcesTests.cpp LIBRARIES genesis_host)

# The config-profile suite: export/import round-trip, the never-elevate-trust
# import rules, and the format gate.
genesis_add_test(NAME config SOURCES tests/extensions/ConfigTests.cpp LIBRARIES genesis_host)

# The install-from-source suite: Dir/Builtin refusals, and the git-clone path
# against a local fixture repository (gated on the `git` binary).
genesis_add_test(NAME source_install SOURCES tests/extensions/SourceInstallTests.cpp LIBRARIES genesis_host)

# The marketplace-catalog suite: parse/validate with a fake fetcher, the
# catalog JSON reply shape, and install-from-catalog (a dir-source entry, so no
# network).
genesis_add_test(NAME catalog SOURCES tests/extensions/CatalogTests.cpp LIBRARIES genesis_host)

# The developer-consent list's hermetic suite: grant/revoke/consented plus the
# atomic JSON round-trip and the corrupt-file-as-empty reading.
genesis_add_test(NAME consent SOURCES tests/extensions/ConsentTests.cpp LIBRARIES genesis_host)

# The Builtin/Curated disable list's hermetic suite: add/remove/disabled plus
# the atomic JSON round-trip and the corrupt-file-as-empty reading.
genesis_add_test(NAME disabled_list SOURCES tests/extensions/DisabledListTests.cpp LIBRARIES genesis_host)

# The removed-list (tombstone) suite: the removed/remove/restore/ids free
# functions plus the atomic JSON round-trip and the corrupt-file-as-empty
# reading.
genesis_add_test(NAME removed_list SOURCES tests/extensions/RemovedListTests.cpp LIBRARIES genesis_host)

# The builtin-seed suite: seeds a temp store from a temp builtin root, skips
# tombstoned and already-installed ids, records Builtin origins, and reports
# manifest-less directories. Hermetic (genesis_host only).
genesis_add_test(NAME seed SOURCES tests/extensions/SeedTests.cpp LIBRARIES genesis_host)

# The native-extension loader's hermetic suite. It dlopens four fixtures built
# as C MODULE libraries beside the test executable, so the suite exercises the
# real dlopen/dlsym path (plus the loader's refusal paths) without any GES or
# Qt. Each fixture is the same C source compiled with a variant macro.
set(GENESIS_EXTENSION_FIXTURES
    extension_fixture_ok
    extension_fixture_abi
    extension_fixture_wrong
    extension_fixture_noentry
    extension_fixture_noinvoke
)
foreach(fixture IN LISTS GENESIS_EXTENSION_FIXTURES)
    add_library(${fixture} MODULE tests/extensions/fixtures/extension_fixture.c)
    target_include_directories(${fixture} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    # Land the .so beside the test executable (the runtime output dir), which is
    # where the suite resolves it from /proc/self/exe.
    set_target_properties(${fixture} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${fixture} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endforeach()
target_compile_definitions(extension_fixture_abi PRIVATE FIXTURE_ABI_MISMATCH)
target_compile_definitions(extension_fixture_wrong PRIVATE FIXTURE_WRONG_ID)
target_compile_definitions(extension_fixture_noentry PRIVATE FIXTURE_NO_ENTRY)
target_compile_definitions(extension_fixture_noinvoke PRIVATE FIXTURE_NO_INVOKE)

genesis_add_test(NAME extension_host SOURCES tests/extensions/NativeExtensionTests.cpp LIBRARIES genesis_host ${CMAKE_DL_LIBS})
add_dependencies(extension_host_tests ${GENESIS_EXTENSION_FIXTURES})

# The extension host's hermetic suite: scans a temp install store, picks the
# highest version per id, gates Developer loads on recorded consent, collects
# menus/panels/pack roots, routes invoke through the fixture library, and
# reports manifest/library refusals. It reuses the same dlopen fixture
# libraries as extension_host, so it needs no GES or Qt.
genesis_add_test(NAME extension_scan SOURCES tests/extensions/ExtensionHostTests.cpp LIBRARIES genesis_host ${CMAKE_DL_LIBS})
add_dependencies(extension_scan_tests ${GENESIS_EXTENSION_FIXTURES})

# The store manager's hermetic suite: the dependency graph, the transitive
# disable/remove cascade, and the origin-gated remove/restore switch. Uses
# data-only manifests (no library), so it needs no dlopen fixtures.
genesis_add_test(NAME manager SOURCES tests/extensions/ManagerTests.cpp LIBRARIES genesis_host)

genesis_add_test(NAME job SOURCES tests/jobs/JobTests.cpp LIBRARIES genesis_host)
genesis_add_test(NAME model_store SOURCES tests/ai/ModelStoreTests.cpp LIBRARIES genesis_host)
# The gating suite links genesis_ai (always built): in the default build it
# asserts the caption provider reports "disabled"; with the runtime it asserts
# the provider reports "enabled" and a missing model fails gracefully.
genesis_add_test(NAME whisper_gating SOURCES tests/ai/WhisperGatingTests.cpp LIBRARIES genesis_ai)
# The TTS gating suite links genesis_ai (always built): in the default build it
# asserts the speech provider reports "disabled"; with the runtime it asserts
# "enabled" and a missing model fails gracefully.
genesis_add_test(NAME tts_gating SOURCES tests/ai/SherpaTtsGatingTests.cpp LIBRARIES genesis_ai)
# The TTS provider suite runs a real synthesis, so it is registered only when
# the sherpa-onnx runtime is compiled in; without a model on disk it skips
# cleanly.
if(GENESIS_AI_TTS)
    genesis_add_test(NAME tts_provider SOURCES tests/ai/SherpaTtsProviderTests.cpp LIBRARIES genesis_ai)
endif()
# The provider suite runs a real decode, so it is registered only when the
# whisper runtime is compiled in; without a model on disk it skips cleanly.
if(GENESIS_AI_CAPTIONS)
    genesis_add_test(NAME whisper_provider SOURCES tests/ai/WhisperProviderTests.cpp LIBRARIES genesis_ai)
endif()
# The vision gating suite links genesis_ai (always built): in the default build
# it asserts the segmentation provider reports "disabled"; with the runtime it
# asserts "enabled" and a missing model fails gracefully.
genesis_add_test(NAME segmentation_gating SOURCES tests/ai/SegmentationGatingTests.cpp LIBRARIES genesis_ai)
# The mask-store suite is hermetic (genesis_host only): it exercises keying,
# freshness and reuse with fake data.
genesis_add_test(NAME mask_store SOURCES tests/ai/MaskStoreTests.cpp LIBRARIES genesis_host)
# The mask-generation driver's suite is hermetic (genesis_ai only): it drives
# generate_masks with synthetic frames and a fake segment seam, so it runs in
# both the default and the GENESIS_AI_VISION build without a model on disk.
genesis_add_test(NAME mask_driver SOURCES tests/ai/MaskDriverTests.cpp LIBRARIES genesis_ai)
# The manifest suite pins the vision rows' provenance (url/digest/size/licence)
# so a drift in the pinned catalogue fails the build.
genesis_add_test(NAME model_manifest SOURCES tests/ai/ModelManifestTests.cpp LIBRARIES genesis_host)
# The execution-provider suite is hermetic (genesis_ai only): it pins the
# provider naming/preference pure functions and the ai_execution_mode() status
# contract ("CPU" when the ONNX runtime is not compiled in), so it runs in both
# the default and the GENESIS_AI_VISION build.
genesis_add_test(NAME execution_provider SOURCES tests/ai/ExecutionProviderTests.cpp LIBRARIES genesis_ai)
# The provider suite runs a real forward pass, so it is registered only when
# the ONNX runtime is compiled in; without a model on disk it skips cleanly.
if(GENESIS_AI_VISION)
    genesis_add_test(NAME segmentation_provider SOURCES tests/ai/SegmentationProviderTests.cpp LIBRARIES genesis_ai)
endif()
# The smart-brush gating suite links genesis_ai (always built): in the default
# build it asserts the brush provider reports "disabled" and the pure helpers
# (brush_input_size/brush_combine) behave; with the runtime it asserts
# "enabled" and a missing model fails gracefully.
genesis_add_test(NAME brush_gating SOURCES tests/ai/BrushGatingTests.cpp LIBRARIES genesis_ai)
# The brush provider suite runs a real encoder/decoder forward pass, so it is
# registered only when the ONNX runtime is compiled in; without a model on disk
# it skips cleanly.
if(GENESIS_AI_VISION)
    genesis_add_test(NAME brush_provider SOURCES tests/ai/BrushProviderTests.cpp LIBRARIES genesis_ai)
endif()
# The CLI's verbs live in Commands.cpp so they are callable as functions (the
# test drives them with a stringstream); the executable is a thin CLI11 shell.
genesis_add_test(NAME cli SOURCES src/cli/Commands.cpp src/cli/JsonRpc.cpp tests/cli/CommandsTests.cpp LIBRARIES genesis_engine_ges genesis_host CLI11::CLI11)
genesis_add_test(NAME media_cache SOURCES tests/render/MediaCacheTests.cpp LIBRARIES genesis_host)
genesis_add_test(NAME reverse_cache SOURCES tests/render/ReverseCacheTests.cpp LIBRARIES genesis_host)
genesis_add_test(NAME proxy_cache SOURCES tests/render/ProxyCacheTests.cpp LIBRARIES genesis_host)
genesis_add_test(NAME exportspec SOURCES tests/render/ExportSpecTests.cpp LIBRARIES genesis_host)
genesis_add_test(NAME transitions SOURCES tests/render/TransitionsTests.cpp LIBRARIES genesis_host)
genesis_add_test(NAME api SOURCES tests/api/ApiTests.cpp LIBRARIES genesis_host)
# The socket_serve suite compiles SocketServe.cpp (the Unix-socket transport)
# with Commands.cpp and JsonRpc.cpp (the shared JSON-RPC codec) so it drives a
# real fork + Unix socket round-trip plus the codec directly, without spawning
# the binary.
genesis_add_test(NAME socket_serve SOURCES src/cli/SocketServe.cpp src/cli/Commands.cpp src/cli/JsonRpc.cpp tests/cli/SocketServeTests.cpp LIBRARIES genesis_engine_ges genesis_host)
# The gRPC server suite: forks a real server on an ephemeral localhost port,
# round-trips a version request through the gRPC Call RPC and compares it
# byte-for-byte against the stdio codec, then checks the auth refusal and the
# malformed-JSON path. It compiles GrpcServe.cpp (the gRPC transport) with
# Commands.cpp and JsonRpc.cpp (the shared codec) and links the generated proto
# stub, so it is registered only when GENESIS_GRPC is enabled.
if(GENESIS_GRPC_ENABLED)
    genesis_add_test(NAME grpc_serve
        SOURCES src/cli/GrpcServe.cpp src/cli/Commands.cpp src/cli/JsonRpc.cpp tests/cli/GrpcServeTests.cpp
        LIBRARIES genesis_engine_ges genesis_host genesis_grpc_proto)
endif()
genesis_add_test(NAME pack_source SOURCES tests/effects/PackSourceTests.cpp LIBRARIES genesis_host WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
genesis_add_test(NAME catalogue SOURCES tests/render/CatalogueTests.cpp LIBRARIES genesis_host WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
genesis_add_test(NAME title SOURCES tests/render/TitleTests.cpp LIBRARIES genesis_host)
genesis_add_test(NAME title_requests SOURCES tests/render/TitleRequestTests.cpp LIBRARIES genesis_host)
genesis_add_test(NAME workspace SOURCES tests/workspace/WorkspaceTests.cpp LIBRARIES genesis_host)
# The self-update core's hermetic suite: manifest parse/reject, digest-mismatch
# and interrupted-download cleanup, staged install + atomic swap, rollback and
# the disabled-clean gate. Runs in the default build (a stub verifier drives the
# crypto-free pipeline, so no OpenSSL is needed).
genesis_add_test(NAME updater SOURCES tests/workspace/UpdaterTests.cpp LIBRARIES genesis_host)
# The Ed25519 sign/verify suite, registered only when the updater is enabled:
# it generates a key pair in-process and needs OpenSSL's EVP (which genesis_host
# only links, and only propagates, when GENESIS_UPDATER=ON and OpenSSL was
# found).
if(GENESIS_UPDATER_ENABLED)
    genesis_add_test(NAME updater_crypto SOURCES tests/workspace/UpdaterCryptoTests.cpp LIBRARIES genesis_host OpenSSL::Crypto)
endif()
genesis_add_test(NAME reveal_pack SOURCES tests/effects/RevealPackTests.cpp LIBRARIES genesis_host WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
genesis_add_test(NAME pack_batch SOURCES tests/effects/PackBatchTests.cpp LIBRARIES genesis_host WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# GES adapter suites: drive the real host -> flatten -> resolve -> GES
# pipeline headlessly, so they link the adapter (and transitively the
# GStreamer/GES flags) rather than the engine-free host library alone.
genesis_add_test(NAME ges_builder SOURCES tests/adapters/GesBuilderTests.cpp LIBRARIES genesis_engine_ges)
genesis_add_test(NAME ges_session SOURCES tests/adapters/GesSessionTests.cpp LIBRARIES genesis_engine_ges)
genesis_add_test(NAME prepare SOURCES tests/adapters/PrepareTests.cpp LIBRARIES genesis_engine_ges)
genesis_add_test(NAME probe SOURCES tests/adapters/ProbeTests.cpp LIBRARIES genesis_engine_ges)
genesis_add_test(NAME hardware_probe SOURCES tests/adapters/HardwareTests.cpp LIBRARIES genesis_engine_ges)
# The encoder-selection suite pins the shared HEVC candidate table and the
# injected-predicate resolver. Pure (no registry), so it exercises the
# engine-free half of the extracted Encoder module.
genesis_add_test(NAME encoder SOURCES tests/adapters/EncoderTests.cpp LIBRARIES genesis_engine_ges)
genesis_add_test(NAME frame_texture SOURCES tests/adapters/FrameTextureTests.cpp LIBRARIES genesis_engine_ges)
genesis_add_test(NAME audio_output SOURCES tests/adapters/AudioOutputTests.cpp LIBRARIES genesis_engine_ges)
genesis_add_test(NAME audio_frames SOURCES tests/adapters/AudioFramesTests.cpp LIBRARIES genesis_engine_ges)
genesis_add_test(NAME video_frames SOURCES tests/adapters/VideoFramesTests.cpp LIBRARIES genesis_engine_ges)
genesis_add_test(NAME proxy SOURCES tests/adapters/ProxyTests.cpp LIBRARIES genesis_engine_ges)
genesis_add_test(NAME decode_preference SOURCES tests/adapters/DecodePreferenceTests.cpp LIBRARIES genesis_engine_ges)
genesis_add_test(NAME shader_chain SOURCES tests/adapters/ShaderChainTests.cpp LIBRARIES genesis_engine_ges)
genesis_add_test(NAME effect_application SOURCES tests/adapters/EffectApplicationTests.cpp LIBRARIES genesis_engine_ges WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
genesis_add_test(NAME filmstrip SOURCES tests/adapters/FilmstripTests.cpp LIBRARIES genesis_engine_ges)
genesis_add_test(NAME export SOURCES tests/adapters/ExportTests.cpp LIBRARIES genesis_engine_ges)
genesis_add_test(NAME scopes SOURCES tests/adapters/ScopesTests.cpp LIBRARIES genesis_engine_ges)
genesis_add_test(NAME reverse SOURCES tests/adapters/ReverseTests.cpp LIBRARIES genesis_engine_ges)
genesis_add_test(NAME reverse_builder SOURCES tests/adapters/ReverseBuilderTests.cpp LIBRARIES genesis_engine_ges)
genesis_add_test(NAME proxy_builder SOURCES tests/adapters/ProxyBuilderTests.cpp LIBRARIES genesis_engine_ges)
genesis_add_test(NAME texture_mix SOURCES tests/adapters/TextureMixTests.cpp LIBRARIES genesis_engine_ges)
genesis_add_test(NAME reveal_bind SOURCES tests/adapters/RevealBindTests.cpp LIBRARIES genesis_engine_ges)
genesis_add_test(NAME lut_grade SOURCES tests/adapters/LutGradeTests.cpp LIBRARIES genesis_engine_ges)
# The clip-effect suite drives install_clip_effects/configure_clip_effects
# through a hand-built GES timeline and captures RGBA pixels. Needs the source
# dir as cwd so the builtin catalogue finds the builtin packs.
genesis_add_test(NAME clip_effect SOURCES tests/adapters/ClipEffectTests.cpp LIBRARIES genesis_engine_ges WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
# The chain-consumption suite drives the real production path (Editor
# AddMedia/AddClip/UpdateClip -> flatten -> render::build_timeline ->
# ges::build_timeline) and captures RGBA, proving an ordinary clip's effect
# chain renders through the builder rather than a hand-built graph. Same link
# and cwd as the clip-effect suite.
genesis_add_test(NAME chain_consumption SOURCES tests/adapters/ChainConsumptionTests.cpp LIBRARIES genesis_engine_ges WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
# The reveal-effect suite drives the custom reveal effect end-to-end through a
# real GES pipeline with a self-created EGL context, proving the reveal map
# masks the picture and survives a flush seek. It builds the reveal pass
# directly (no pack resolution), so it needs no source-dir cwd.
genesis_add_test(NAME reveal_effect SOURCES tests/adapters/RevealEffectTests.cpp LIBRARIES genesis_engine_ges)
# The mask-effect suite drives the per-frame mask effect end-to-end through a
# real GES pipeline with a self-created EGL context, proving the mask modulates
# the picture's alpha frame by frame. It builds the mask pass directly (no pack
# resolution) and writes its own mask files to a temp dir, so it needs no
# source-dir cwd.
genesis_add_test(NAME mask_effect SOURCES tests/adapters/MaskEffectTests.cpp LIBRARIES genesis_engine_ges)

# The five folded packs need the adapter linked: their suite compiles the emitted
# fragment source through a real glshader and asserts causal pixels, not strings.
genesis_add_test(NAME named_intermediate SOURCES tests/effects/NamedIntermediateTests.cpp LIBRARIES genesis_engine_ges WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
# Host-only suites: title_templates and presets walk up from cwd to find
# assets/ and the builtin packs, so they run with the source dir as cwd.
genesis_add_test(NAME lut_loader SOURCES tests/render/LutLoaderTests.cpp LIBRARIES genesis_host)
genesis_add_test(NAME title_templates SOURCES tests/render/TitleTemplatesTests.cpp LIBRARIES genesis_host WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
genesis_add_test(NAME presets SOURCES tests/render/PresetsTests.cpp LIBRARIES genesis_host WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
genesis_add_test(NAME export_presets SOURCES tests/render/ExportPresetsTests.cpp LIBRARIES genesis_host)

# The first host GUI: a Qt Quick monitor window that plays a host-built
# timeline through the GES session and presents its frames. Only this target
# sees both Qt and GES; genesis_host stays Qt-free and engine-free (R5/R6) and
# genesis_engine_ges stays Qt-free.
# Qt6GuiPrivate exists from Qt 6.5 onward; before that there is no private-module
# config and no <rhi/qrhi.h>, so the app cannot be built at all. The host, the
# engine adapter and most suites never touch Qt, so an older Qt degrades the
# build to them with a clear warning instead of failing outright.
find_package(Qt6 QUIET COMPONENTS Core Network Gui Quick Qml GuiPrivate)
set(GENESIS_CAN_BUILD_QT_TARGETS OFF)
if(TARGET Qt6::GuiPrivate AND TARGET Qt6::Quick AND TARGET Qt6::Qml AND TARGET Qt6::Gui AND TARGET Qt6::Network)
    set(GENESIS_CAN_BUILD_QT_TARGETS ON)
endif()

if(GENESIS_CAN_BUILD_QT_TARGETS)
qt_standard_project_setup()

qt_add_executable(genesis_app
    src/app/GenesisApp.cpp
    src/app/AppShell.h
    src/app/AppShell.cpp
)

# Theme.qml carries `pragma Singleton`. CMake only registers a QML singleton
# when told to, and the property must be set BEFORE qt_add_qml_module reads the
# file list - setting it afterwards leaves the generated qmldir without the
# `singleton` marker, so every `Theme.token` reference resolves to undefined.
set_source_files_properties(src/app/qml/Theme.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(genesis_app
    URI GenesisApp
    VERSION 1.0
    # Without an explicit prefix the module lands at qrc:/GenesisApp (the
    # pre-6.5 QTP0001 default, since qt_standard_project_setup() above is called
    # with no REQUIRES), which is not on the engine's default import path. An
    # extension panel that `import GenesisApp 1.0` at runtime then fails to
    # resolve the pure-QML types. The standard /qt/qml prefix is searched by
    # default, so placing the module there keeps it importable.
    RESOURCE_PREFIX /qt/qml
    QML_FILES
        src/app/qml/Main.qml
        src/app/qml/ClipList.qml
        src/app/qml/ClipInspector.qml
        src/app/qml/EffectParamsSection.qml
        src/app/qml/CutoutSection.qml
        src/app/qml/CutoutStrokeOverlay.qml
        src/app/qml/EnhanceSection.qml
        src/app/qml/ToggleButton.qml
        src/app/qml/TimelineStrip.qml
        src/app/qml/TransportBar.qml
        src/app/qml/ExportDialog.qml
        src/app/qml/KeyframesPane.qml
        src/app/qml/MediaBin.qml
        src/app/qml/Theme.qml
        src/app/qml/Panel.qml
        src/app/qml/Button.qml
        src/app/qml/LabelledSlider.qml
        src/app/qml/InputField.qml
        src/app/qml/StartScreen.qml
        src/app/qml/SettingsDialog.qml
        src/app/qml/ExtensionsDialog.qml
        src/app/qml/ScopesPane.qml
        src/app/qml/CaptionsDialog.qml
        src/app/qml/CutoutDialog.qml
        src/app/qml/EnhanceDialog.qml
        src/app/qml/SpeechDialog.qml
        src/app/qml/GpuIndicator.qml
    SOURCES
        src/app/support/AppLog.h
        src/app/support/AppLog.cpp
        src/app/net/QtFetcher.h
        src/app/net/QtFetcher.cpp
        src/app/Version.h
        src/app/controllers/AppSettings.h
        src/app/controllers/AppSettings.cpp
        src/app/controllers/AiController.h
        src/app/controllers/AiController.cpp
        src/app/controllers/CutoutController.h
        src/app/controllers/CutoutController.cpp
        src/app/controllers/EnhanceController.h
        src/app/controllers/EnhanceController.cpp
        src/app/controllers/TtsController.h
        src/app/controllers/TtsController.cpp
        src/app/controllers/UpdateController.h
        src/app/controllers/UpdateController.cpp
        src/app/controllers/ExtensionsController.h
        src/app/controllers/ExtensionsController.cpp
        src/app/model/MonitorItem.h
        src/app/model/MonitorItem.cpp
        src/app/model/TimelineModel.h
        src/app/model/TimelineModel.cpp
        src/app/controllers/EditController.h
        src/app/controllers/EditController.cpp
        src/app/controllers/EffectParamsController.h
        src/app/controllers/EffectParamsController.cpp
        src/app/render/BuildForEngine.h
        src/app/render/BuildForEngine.cpp
        src/app/controllers/ExportController.h
        src/app/controllers/ExportController.cpp
        src/app/controllers/HardwareStatus.h
        src/app/controllers/HardwareStatus.cpp
        src/app/support/I18n.h
        src/app/support/I18n.cpp
        src/app/controllers/MediaBinController.h
        src/app/controllers/MediaBinController.cpp
        src/app/controllers/KeyframesController.h
        src/app/controllers/KeyframesController.cpp
        src/app/controllers/PlaybackController.h
        src/app/controllers/PlaybackController.cpp
        src/app/controllers/ProjectController.h
        src/app/controllers/ProjectController.cpp
        src/app/controllers/OffThreadGenerator.h
        src/app/controllers/OffThreadGenerator.cpp
        src/app/controllers/ProxyController.h
        src/app/controllers/ProxyController.cpp
        src/app/controllers/ReverseController.h
        src/app/controllers/ReverseController.cpp
        src/app/controllers/ScopesController.h
        src/app/controllers/ScopesController.cpp
        src/app/render/TitleRenderer.h
        src/app/render/TitleRenderer.cpp
)

# The generated QML type registration does `#if __has_include(<MonitorItem.h>)`,
# so the model directory (where MonitorItem.h lives) has to be on the include
# path or the registered types silently vanish from the module.
target_include_directories(genesis_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/app/model)

target_link_libraries(genesis_app PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Gui
    Qt6::GuiPrivate
    Qt6::Quick
    Qt6::Qml
    PkgConfig::GST
    ges-1.0
    genesis_ai
    genesis_engine_ges
    genesis_host
)

genesis_warnings(genesis_app)

# The QML file is loaded from the source tree (the spike pattern), so the app
# stays a single tree and the scene is editable without a rebuild.
target_compile_definitions(genesis_app PRIVATE
    GENESIS_QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/app/qml"
    GENESIS_APP_VERSION="${PROJECT_VERSION}"
)

# The binary lands in build/bin/ alongside genesis-cli, so both executables
# share the CMAKE_RUNTIME_OUTPUT_DIRECTORY set above.
set_target_properties(genesis_app PROPERTIES
    OUTPUT_NAME "genesis-app"
)

# Ship the builtin extension source beside the app binary too, so the packaged
# app's startup seed finds it via builtin_root()'s beside-the-exe fallback.
add_custom_command(TARGET genesis_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/extensions/builtin
        $<TARGET_FILE_DIR:genesis_app>/extensions/builtin
    VERBATIM)

# Qt-controller suites. Each compiles a src/app/controllers/*.cpp controller
# together with its test driver and puts src/app/controllers on the include
# path (the controllers' headers live there). They are declared after
# qt_standard_project_setup() enabled AUTOMOC, which the QObject-based
# controllers depend on.
genesis_add_test(NAME edit_controller
    SOURCES src/app/controllers/EditController.cpp tests/app/EditControllerTests.cpp
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/app/controllers
    LIBRARIES Qt6::Core genesis_host)
genesis_add_test(NAME title_renderer
    SOURCES src/app/render/TitleRenderer.cpp tests/app/TitleRendererTests.cpp
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src
    LIBRARIES Qt6::Core Qt6::Gui genesis_host)
# The scopes controller's suite needs both roots: the controllers dir for its
# own header, and src for the engine seam's Scopes.h that the injected source's
# type comes from.
genesis_add_test(NAME scopes_controller
    SOURCES src/app/controllers/ScopesController.cpp tests/app/ScopesControllerTests.cpp
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/app/controllers
    LIBRARIES Qt6::Core)
# The hardware-status controller's suite is hermetic: it injects the probe
# result as plain values, so it needs no GStreamer or GL, only Qt Core and the
# controller. The src include root resolves the controller's own header.
genesis_add_test(NAME hardware_status
    SOURCES src/app/controllers/HardwareStatus.cpp tests/app/HardwareStatusTests.cpp
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/app/controllers
    LIBRARIES Qt6::Core)
genesis_add_test(NAME export_controller
    SOURCES src/app/controllers/ExportController.cpp tests/app/ExportControllerTests.cpp
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/app/controllers
    LIBRARIES Qt6::Core genesis_engine_ges)
genesis_add_test(NAME media_bin
    SOURCES src/app/controllers/MediaBinController.cpp tests/app/MediaBinTests.cpp
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/app/controllers
    LIBRARIES Qt6::Core genesis_engine_ges)
genesis_add_test(NAME reverse_controller
    SOURCES src/app/controllers/ReverseController.cpp src/app/controllers/OffThreadGenerator.cpp tests/app/ReverseControllerTests.cpp
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/app/controllers
    LIBRARIES Qt6::Core genesis_engine_ges)
genesis_add_test(NAME proxy_controller
    SOURCES src/app/controllers/ProxyController.cpp src/app/controllers/OffThreadGenerator.cpp tests/app/ProxyControllerTests.cpp
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/app/controllers
    LIBRARIES Qt6::Core genesis_engine_ges)
genesis_add_test(NAME keyframes_controller
    SOURCES src/app/controllers/KeyframesController.cpp tests/app/KeyframesControllerTests.cpp
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/app/controllers
    LIBRARIES Qt6::Core genesis_host)
# The effect-parameter controller's suite resolves the real shipped packs
# through the builtin catalogue, so it runs with the source dir as cwd.
genesis_add_test(NAME effect_params_controller
    SOURCES src/app/controllers/EffectParamsController.cpp tests/app/EffectParamsControllerTests.cpp
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/app/controllers
    LIBRARIES Qt6::Core genesis_host
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
# The AI captions controller's suite: drives consent -> install -> extract ->
# transcribe -> apply headlessly with injected fakes (no model download, no
# GStreamer decode, no whisper run). It compiles the controller and the real
# QtFetcher (QNetworkAccessManager) and links the engine adapter (the default
# extractor reaches ges::extract_audio) and genesis_ai (the default transcriber
# reaches WhisperProvider), so it runs in both the default and the
# GENESIS_AI_CAPTIONS build - the runtime-gated tests skip in the default build.
genesis_add_test(NAME ai_controller
    SOURCES src/app/controllers/AiController.cpp src/app/net/QtFetcher.cpp tests/app/AiControllerTests.cpp
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/app/controllers
    LIBRARIES Qt6::Core Qt6::Network genesis_ai genesis_engine_ges)
# The AI cutout controller's suite: drives consent -> install -> decode ->
# segment -> apply headlessly with injected fakes (no model download, no
# GStreamer decode, no ONNX run). It compiles the controller and the real
# QtFetcher and links genesis_engine_ges (the default frame source reaches
# ges::extract_video_frames) and genesis_ai (the default segmenter reaches
# SegmentationProvider), so it runs in both the default and the
# GENESIS_AI_VISION build - the runtime-gated tests skip in the default build.
genesis_add_test(NAME cutout_controller
    SOURCES src/app/controllers/CutoutController.cpp src/app/net/QtFetcher.cpp tests/app/CutoutControllerTests.cpp
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/app/controllers
    LIBRARIES Qt6::Core Qt6::Network genesis_ai genesis_engine_ges)
# The AI enhance controller's suite: drives consent -> install -> decode ->
# enhance -> encode -> apply headlessly with injected fakes (no model download,
# no GStreamer decode/encode, no ONNX run). It compiles the controller and the
# real QtFetcher and links genesis_engine_ges (the default frame source reaches
# ges::extract_video_frames and the default encoder is a GStreamer pipeline) and
# genesis_ai (the default enhancer reaches EnhanceProvider), so it runs in both
# the default and the GENESIS_AI_VISION build - the runtime-gated tests skip in
# the default build.
genesis_add_test(NAME enhance_controller
    SOURCES src/app/controllers/EnhanceController.cpp src/app/net/QtFetcher.cpp tests/app/EnhanceControllerTests.cpp
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/app/controllers
    LIBRARIES Qt6::Core Qt6::Network genesis_ai genesis_engine_ges)
# The TTS controller's suite: drives consent -> install -> extract ->
# synthesize -> apply headlessly with injected fakes (no model download, no
# tar extraction, no sherpa-onnx run). It compiles the controller and the real
# QtFetcher and links genesis_ai (the default synthesizer reaches
# SherpaTtsProvider), so it runs in both the default and the GENESIS_AI_TTS
# build - the runtime-gated tests skip in the default build.
genesis_add_test(NAME tts_controller
    SOURCES src/app/controllers/TtsController.cpp src/app/net/QtFetcher.cpp tests/app/TtsControllerTests.cpp
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src/app/controllers
    LIBRARIES Qt6::Core Qt6::Network genesis_ai)
# The update controller's suite: drives the whole state machine headlessly with
# injected fakes (a stub feed fetcher, a stub signature verifier, and a gate),
# so the notify-only surface is fully testable in a build without the updater
# runtime. It compiles the real controller and links genesis_host for the
# manifest/signature types. The default fetcher uses Qt Network, hence
# Qt6::Network even though the tests never touch the network.
genesis_add_test(NAME update_controller
    SOURCES src/app/controllers/UpdateController.cpp tests/app/UpdateControllerTests.cpp
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/app/controllers
    LIBRARIES Qt6::Core Qt6::Network genesis_host)

# The extension controller's suite: drives scan -> project -> consent grant/
# revoke -> invoke headlessly against a temp install store holding the dlopen
# fixture library, with the real api::dispatch bridge over a real Editor. It
# compiles the controller plus the app's shared log sink (the controller's
# host.log routes refusal lines through app_log) and links genesis_host for the
# host/api/editor seams. No GStreamer or GL.
genesis_add_test(NAME extensions_controller
    SOURCES src/app/controllers/ExtensionsController.cpp src/app/support/AppLog.cpp tests/app/ExtensionsControllerTests.cpp
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/app/controllers
    LIBRARIES Qt6::Core genesis_host)
add_dependencies(extensions_controller_tests ${GENESIS_EXTENSION_FIXTURES})

else()
    message(WARNING
        "Qt >= 6.5 with the GuiPrivate component was not found (found '${Qt6_VERSION}'). "
        "The Qt application and its 16 suites are SKIPPED; the host, the engine adapter "
        "and the other suites still build and test. See docs/building.md.")
endif()

# The perf suite: a benchmark driver with its own main. Benchmarks must run at
# -O2 - a -O0 benchmark measures nothing - so this target overrides the default
# build type's optimization level, and it compiles the harness directly rather
# than through genesis_host so benchmark code does not ship in the library.
genesis_add_test(NAME perf
    SOURCES src/perf/Benchmarks.cpp tests/perf/BenchmarkTests.cpp
    LIBRARIES genesis_host
    COMPILE_OPTIONS -O2)

# ---------------------------------------------------------------------------
# Code formatting (clang-format) and static analysis (clang-tidy, cppcheck)
# ---------------------------------------------------------------------------
# The official style is `.clang-format` (FormatStyle: file) and the check set
# is `.clang-tidy`. The `format` target rewrites in place; the `format_check`
# suite is a ctest test that dry-runs `clang-format --Werror` over the same
# files and fails naming every file that would be reformatted - that is what
# CI enforces. The `tidy` target is a manual, report-only clang-tidy pass (not
# registered with ctest).
#
# Both tools are optional at configure time, matching the repo's degradation
# pattern (Qt, AI, gRPC, updater): find_program, and if the tool is absent the
# target/test are simply not registered, with a warning. clang-format's output
# is NOT stable across major versions, so the formatter must match the version
# the tree was formatted with (22.1.8); see docs/building.md.

find_program(CLANG_FORMAT NAMES clang-format clang-format-22)
find_program(CLANG_TIDY NAMES clang-tidy clang-tidy-22)
find_program(CPPCHECK NAMES cppcheck)

# The files the formatter/linter cover: tracked C++ under src/ and tests/ only.
# spikes/, examples/, build*/ and every FetchContent _deps/ tree all live
# outside those two roots, so scoping the glob to src/ and tests/ excludes them
# by construction. Unlike the explicit compile lists, this set is globbed (with
# CONFIGURE_DEPENDS so ninja re-globs when a file appears): it is a lint set,
# not a build input, and a hand-maintained list would drift the moment a file
# is added or renamed.
file(GLOB_RECURSE GENESIS_FORMAT_SOURCES
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    CONFIGURE_DEPENDS
    src/*.h src/*.hh src/*.hpp src/*.hxx
    src/*.cpp src/*.cc src/*.cxx
    tests/*.h tests/*.hh tests/*.hpp tests/*.hxx
    tests/*.cpp tests/*.cc tests/*.cxx)

if(CLANG_FORMAT)
    # Rewrite the whole tree in place, per .clang-format.
    add_custom_target(format
        COMMAND ${CLANG_FORMAT} -i ${GENESIS_FORMAT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Formatting C++ sources with clang-format"
        VERBATIM)
    # The ctest suite CI runs: a dry-run that fails (naming each offending
    # file) if the tree is not already formatted. Registered only when the
    # tool exists at configure time, so an absent formatter is a clean skip,
    # not a failure.
    add_test(NAME format_check
        COMMAND ${CLANG_FORMAT} --dry-run --Werror ${GENESIS_FORMAT_SOURCES})
    set_tests_properties(format_check PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
else()
    message(WARNING
        "clang-format was not found. The `format` target and the `format_check` "
        "suite are SKIPPED; install clang-format 22.1.8 to enable them. See "
        "docs/building.md.")
endif()

# Manual, report-only clang-tidy pass. It keys off compile_commands.json (the
# CMAKE_EXPORT_COMPILE_COMMANDS setting above) and reads .clang-tidy for the
# check set. Findings are printed, not enforced - no --warnings-as-errors - and
# it is NOT registered with ctest: run it deliberately when wanted. Headers are
# covered transitively through each translation unit via HeaderFilterRegex, so
# the pass runs over the .cpp translation units.
file(GLOB_RECURSE GENESIS_TIDY_SOURCES
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    CONFIGURE_DEPENDS
    src/*.cpp src/*.cc src/*.cxx
    tests/*.cpp tests/*.cc tests/*.cxx)
if(CLANG_TIDY)
    add_custom_target(tidy
        COMMAND ${CLANG_TIDY} -p ${CMAKE_BINARY_DIR} ${GENESIS_TIDY_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running clang-tidy (report-only)"
        VERBATIM)
else()
    message(WARNING
        "clang-tidy was not found. The manual `tidy` target is SKIPPED; "
        "install clang-tidy to enable it. See docs/building.md.")
endif()

# Manual, report-only cppcheck pass - the third C++ tool alongside clang-format
# and clang-tidy. It runs its own preprocessor (not compile_commands.json), so
# it needs only `-I src` for the project's include root, not a successful
# configure of every translation unit. Qt/GStreamer system headers are
# deliberately NOT passed (collecting their include dirs is not cheap, and the
# missingInclude category is not enabled), so third-party headers go unresolved
# - an accepted coverage reduction for a baseline pass.
#
# Report-only by design: --error-exitcode=0 means findings never fail the
# build, and it is NOT registered with ctest. Suppressions live in
# `.cppcheck-suppressions` (--suppressions-list) and inline
# `// cppcheck-suppress <id>` comments (--inline-suppr). Scope is src/ only;
# build*/, _deps/, spikes/ and examples/ are excluded by the glob and again by
# the -i ignores.
file(GLOB_RECURSE GENESIS_CPPCHECK_SOURCES
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    CONFIGURE_DEPENDS
    src/*.cpp src/*.cc src/*.cxx)
if(CPPCHECK)
    # --std=c++23 was added in cppcheck 2.11; probe for it so an older cppcheck
    # degrades to its own default standard instead of erroring on the flag.
    execute_process(
        COMMAND ${CPPCHECK} --std=c++23 --version
        RESULT_VARIABLE CPPCHECK_CXX23_OK
        OUTPUT_QUIET ERROR_QUIET)
    set(GENESIS_CPPCHECK_ARGS
        --enable=warning,performance,portability
        --inline-suppr
        --quiet
        --error-exitcode=0
        --suppressions-list=${CMAKE_CURRENT_SOURCE_DIR}/.cppcheck-suppressions
        -I ${CMAKE_CURRENT_SOURCE_DIR}/src
        -i build* -i _deps -i spikes -i examples)
    if(CPPCHECK_CXX23_OK EQUAL 0)
        list(APPEND GENESIS_CPPCHECK_ARGS --std=c++23)
    endif()
    add_custom_target(cppcheck
        COMMAND ${CPPCHECK} ${GENESIS_CPPCHECK_ARGS} ${GENESIS_CPPCHECK_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running cppcheck (report-only)"
        VERBATIM)
else()
    message(WARNING
        "cppcheck was not found. The manual `cppcheck` target is SKIPPED; "
        "install cppcheck to enable it. See docs/building.md.")
endif()

# ---------------------------------------------------------------------------
# QML formatting (qmlformat)
# ---------------------------------------------------------------------------
# The official style is `.qmlformat.ini` (NormalizeOrder + 100-column wrap +
# LF). The `qml_format` target rewrites the QML in place; the `qml_format_check`
# suite is the ctest that CI enforces: it dry-runs `qmlformat` over the same
# files and fails naming every file that would be reformatted (the comparison
# lives in cmake/QmlFormatCheck.cmake, since qmlformat has no `--dry-run
# --Werror` like clang-format). Linting is separate and deliberately NOT wired
# as a gate: the `all_qmllint` target generated by qt_add_qml_module runs
# `qmllint` report-only (see docs/building.md for the residual warnings).
#
# Like clang-format, qmlformat is optional at configure time: find_program, and
# if the tool is absent the target/test are not registered, with a warning.
# qmlformat's output is not stable across Qt versions, so the formatter must
# match the Qt the tree was formatted with (6.11); see docs/building.md.

find_program(QMLFORMAT NAMES qmlformat qmlformat-6)
if(NOT QMLFORMAT AND TARGET Qt6::qmlformat)
    # qmlformat is not on PATH (e.g. it lives under /usr/lib/qt6/bin, as on
    # this distro). When Qt is present, the Qt6::qmlformat tool target knows its
    # exact location, so use that instead of failing the find.
    get_target_property(QMLFORMAT Qt6::qmlformat IMPORTED_LOCATION)
endif()

# The files the formatter covers: the app's QML directory and the builtin
# extension panels. This set is globbed (CONFIGURE_DEPENDS) rather than
# hand-maintained, for the same reason as GENESIS_FORMAT_SOURCES above: it is a
# lint/format set, not a build input.
file(GLOB_RECURSE GENESIS_QML_SOURCES
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    CONFIGURE_DEPENDS
    src/app/qml/*.qml
    extensions/builtin/*.qml)

if(QMLFORMAT)
    # Rewrite the QML in place, per .qmlformat.ini (passed explicitly so the
    # target does not depend on qmlformat's directory-hierarchy search).
    add_custom_target(qml_format
        COMMAND ${QMLFORMAT} -s ${CMAKE_CURRENT_SOURCE_DIR}/.qmlformat.ini -i
                ${GENESIS_QML_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Formatting QML sources with qmlformat"
        VERBATIM)
    # The ctest suite CI runs: a dry-run comparison that fails (naming each
    # offending file) if the tree is not already formatted. Registered only
    # when the tool exists at configure time, so an absent formatter is a clean
    # skip, not a failure.
    add_test(NAME qml_format_check
        COMMAND ${CMAKE_COMMAND}
            "-DQMLFORMAT=${QMLFORMAT}"
            "-DSETTINGS=${CMAKE_CURRENT_SOURCE_DIR}/.qmlformat.ini"
            "-DGENESIS_QML_SOURCES=${GENESIS_QML_SOURCES}"
            "-DCHECK_TMPDIR=${CMAKE_BINARY_DIR}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/QmlFormatCheck.cmake)
    set_tests_properties(qml_format_check PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
else()
    message(WARNING
        "qmlformat was not found. The `qml_format` target and the "
        "`qml_format_check` suite are SKIPPED; install qmlformat (Qt 6.11) to "
        "enable them. See docs/building.md.")
endif()
