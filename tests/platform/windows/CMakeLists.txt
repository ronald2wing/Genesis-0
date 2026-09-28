cmake_minimum_required(VERSION 3.28)
project(genesis_subprocess_windows LANGUAGES CXX)

# A focused Windows compile-and-run check for src/extensions/Subprocess.cpp.
# The full app is Linux-first (Qt/GStreamer panel), so this harness links only
# the engine-free Subprocess unit plus Boost.Process's headers.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# vcpkg's boost ports are compatible with FindBoost's built-in targets;
# Boost.Process is header-only, so Boost::boost is the only link target.
find_package(Boost REQUIRED)

add_executable(subprocess_check
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/extensions/Subprocess.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
)
target_include_directories(subprocess_check PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src
)
# Asio on Windows needs Winsock.
if(WIN32)
    target_link_libraries(subprocess_check PRIVATE ws2_32)
endif()
