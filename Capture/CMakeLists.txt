cmake_minimum_required(VERSION 3.20)
project(RecordingEngine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Set output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Force Unicode and UTF-8
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/EHsc>")
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/Zc:__cplusplus>")

# Find packages
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

# vcpkg toolchain should set these paths
find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h PATH_SUFFIXES include)
find_library(FFMPEG_avcodec_LIBRARY avcodec)
find_library(FFMPEG_avformat_LIBRARY avformat)
find_library(FFMPEG_avutil_LIBRARY avutil)
find_library(FFMPEG_swscale_LIBRARY swscale)

# DirectX
find_path(DXGI_INCLUDE_DIR dxgi1_6.h PATH_SUFFIXES include)
find_path(D3D12_INCLUDE_DIR d3d12.h PATH_SUFFIXES include)
find_path(D3D11_INCLUDE_DIR d3d11_1.h PATH_SUFFIXES include)

# spdlog
find_path(spdlog_INCLUDE_DIR spdlog/spdlog.h PATH_SUFFIXES include)

# nlohmann_json
find_path(nlohmann_json_INCLUDE_DIR nlohmann/json.hpp PATH_SUFFIXES include)

# Taskflow
find_path(Taskflow_INCLUDE_DIR taskflow/taskflow.hpp PATH_SUFFIXES include)

# ImGui - Find both includes and library
find_path(IMGUI_INCLUDE_DIR 
    NAMES imgui.h
    PATH_SUFFIXES include
)

find_path(IMGUI_BACKENDS_INCLUDE_DIR
    NAMES imgui_impl_win32.h
    PATH_SUFFIXES include/backends
)

find_library(IMGUI_LIBRARY
    NAMES imgui
    PATH_SUFFIXES lib
)

# Include directories
include_directories(
    ${FFMPEG_INCLUDE_DIR}
    ${DXGI_INCLUDE_DIR}
    ${D3D12_INCLUDE_DIR}
    ${D3D11_INCLUDE_DIR}
    ${spdlog_INCLUDE_DIR}
    ${nlohmann_json_INCLUDE_DIR}
    ${Taskflow_INCLUDE_DIR}
    ${IMGUI_INCLUDE_DIR}
    ${IMGUI_BACKENDS_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Add executable
add_executable(RecordingEngine WIN32
    main.cpp
)

# Link libraries
target_link_libraries(RecordingEngine PRIVATE
    ${FFMPEG_avcodec_LIBRARY}
    ${FFMPEG_avformat_LIBRARY}
    ${FFMPEG_avutil_LIBRARY}
    ${FFMPEG_swscale_LIBRARY}
    ${IMGUI_LIBRARY}
    d3d11
    d3d12
    dxgi
    dxguid
    ole32
    oleaut32
    avrt
    winmm
    imm32
    version
    ${OPENSSL_LIBRARIES}
    ${ZLIB_LIBRARIES}
)

# Copy DLLs
add_custom_command(TARGET RecordingEngine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${VCPKG_INSTALLED_PATH}/bin/avcodec-*.dll
        ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${VCPKG_INSTALLED_PATH}/bin/avformat-*.dll
        ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${VCPKG_INSTALLED_PATH}/bin/avutil-*.dll
        ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${VCPKG_INSTALLED_PATH}/bin/swscale-*.dll
        ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/
)