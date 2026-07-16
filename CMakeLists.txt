cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)

# FFmpeg for in-process video/audio decoding (gallery playback, Windows only)
if(WIN32)
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-studio-31.1.1/cmake/finders")
  find_package(FFmpeg 6.1 REQUIRED avcodec avformat avutil swscale swresample)
endif()

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)
if(WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
    FFmpeg::avcodec
    FFmpeg::avformat
    FFmpeg::avutil
    FFmpeg::swscale
    FFmpeg::swresample
  )
endif()

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE ENABLE_FRONTEND_API)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE ENABLE_QT)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/plugin/plugin-support.c.in"
  "${CMAKE_CURRENT_BINARY_DIR}/plugin-support.c"
  @ONLY
)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
  src/plugin/plugin-main.cpp
  src/plugin/overlay-settings-storage.cpp
  src/plugin/overlay-settings-storage.h
  src/plugin/overlay-smart-replay.cpp
  src/plugin/overlay-smart-replay.h
  src/plugin/plugin-support.h
  "${CMAKE_CURRENT_BINARY_DIR}/plugin-support.c"
)

# Windows API implementation (Direct2D)
if(WIN32)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    src/renderer/overlay-renderer.cpp
    src/renderer/overlay-renderer-input.cpp
    src/renderer/overlay-renderer.h
    src/renderer/overlay-gallery.cpp
    src/renderer/overlay-gallery.h
    src/common/overlay-string-utils.cpp
    src/common/overlay-string-utils.h
    src/common/overlay-ffmpeg-time-utils.h
    src/common/overlay-mp4-box-reader.cpp
    src/common/overlay-mp4-box-reader.h
    src/player/overlay-video-player.cpp
    src/player/overlay-video-player.h
    src/trimmer/overlay-trimmer.cpp
    src/trimmer/overlay-trimmer.h
    src/indicators/overlay-indicators.cpp
    src/indicators/overlay-indicators.h
    src/window/overlay-window-manager.cpp
    src/window/overlay-window-manager.h
    src/renderer/overlay-layout.cpp
    src/renderer/overlay-layout.h
    src/renderer/overlay-state.cpp
    src/renderer/overlay-state.h
    src/common/overlay-icons-resource.h
  )
  # Direct2D, DirectWrite, WASAPI (ole32), winmm (timeBeginPeriod)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
    d2d1
    dwrite
    windowscodecs
    mfplat
    mfreadwrite
    mfuuid
    mf
    ole32
    winmm
  )
endif()

# C API interface (always included)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
  src/window/overlay-window-c.cpp
  src/window/overlay-window-c.h
  src/indicators/overlay-indicators-c.cpp
  src/indicators/overlay-indicators-c.h
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# Include dirs for modularized source layout.
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/plugin"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/common"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/player"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/trimmer"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/indicators"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/window"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/ui"
)

# Embed PNG icons in DLL (optional: if data/icons/ exists at configure time)
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data/icons/play.png")
  set(ICONS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/data/icons")
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows/resources/icons.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/overlay-icons.rc"
    @ONLY
  )
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/overlay-icons.rc")
endif()
