cmake_minimum_required(VERSION 3.24)
project(CaptureApp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Force UNICODE for all Windows APIs
add_compile_definitions(UNICODE _UNICODE)

# Find packages installed via vcpkg
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(FFMPEG REQUIRED)

# Recursively locate all project .cpp files
file(GLOB_RECURSE PROJECT_SOURCES 
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

# Exclude duplicate entry points and build directories
list(FILTER PROJECT_SOURCES EXCLUDE REGEX "RecordingApp\\.cpp$")
list(FILTER PROJECT_SOURCES EXCLUDE REGEX "Capture\\.cpp$")
list(FILTER PROJECT_SOURCES EXCLUDE REGEX "/build/")
list(FILTER PROJECT_SOURCES EXCLUDE REGEX "/vcpkg_installed/")
list(FILTER PROJECT_SOURCES EXCLUDE REGEX "/\\.")

# Create the Windows executable
add_executable(${PROJECT_NAME} WIN32 ${PROJECT_SOURCES})

# Include project directories and dependencies
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Capture
    ${CMAKE_CURRENT_SOURCE_DIR}/OpenSourceCapture
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${FFMPEG_INCLUDE_DIRS}
)

# Link all required libraries
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
