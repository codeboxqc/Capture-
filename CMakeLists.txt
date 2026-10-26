cmake_minimum_required(VERSION 3.24)
project(CaptureApp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Packages installed via vcpkg
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(FFMPEG REQUIRED)

# Use main.cpp (note: avoid compiling both main.cpp AND RecordingApp.cpp together 
# if both define WinMain/main to prevent LNK2005 multiple definition errors)
set(SOURCES
    camera.cpp
    mouse.cpp
    main.cpp
    region.cpp
    util.cpp
)

# Build a Windows GUI executable
add_executable(${PROJECT_NAME} WIN32 ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${FFMPEG_INCLUDE_DIRS}
)

# Link Windows System APIs and Third-Party Libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    imgui::imgui
    ${FFMPEG_LIBRARIES}
    d3d11
    d3d12
    dxgi
    d3dcompiler
    ole32
    avrt
    winmm
    setupapi
    cfgmgr32
    mf
    mfplat
    mfreadwrite
    mfuuid
    shlwapi
)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /MP /W3)
endif()