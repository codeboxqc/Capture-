cmake_minimum_required(VERSION 3.24)
project(CaptureApp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find vcpkg packages
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(FFMPEG REQUIRED)

# GLOB all .cpp files recursively from the repository
# This guarantees it finds camera.cpp whether it's in root, src/, OpenSourceCapture/, etc.
file(GLOB_RECURSE PROJECT_SOURCES 
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

# Filter out duplicate main files if both main.cpp and RecordingApp.cpp exist
list(FILTER PROJECT_SOURCES EXCLUDE REGEX "RecordingApp\\.cpp$")
# Exclude build output artifacts if re-run locally
list(FILTER PROJECT_SOURCES EXCLUDE REGEX "/build/")
list(FILTER PROJECT_SOURCES EXCLUDE REGEX "/vcpkg_installed/")

# Find ImGui backend implementations provided by vcpkg
find_path(IMGUI_INCLUDE_DIR "imgui.h")
find_file(IMGUI_WIN32_SRC "imgui_impl_win32.cpp" PATHS "${IMGUI_INCLUDE_DIR}/backends" "${IMGUI_INCLUDE_DIR}" REQUIRED)
find_file(IMGUI_DX11_SRC "imgui_impl_dx11.cpp" PATHS "${IMGUI_INCLUDE_DIR}/backends" "${IMGUI_INCLUDE_DIR}" REQUIRED)

list(APPEND PROJECT_SOURCES ${IMGUI_WIN32_SRC} ${IMGUI_DX11_SRC})

# Create executable
add_executable(${PROJECT_NAME} WIN32 ${PROJECT_SOURCES})

# Include all subdirectories where headers might reside
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/OpenSourceCapture
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${FFMPEG_INCLUDE_DIRS}
    ${IMGUI_INCLUDE_DIR}
)

# Link all system and third-party libraries
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
