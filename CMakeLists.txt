cmake_minimum_required(VERSION 3.20)
project(in_hls LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Build for Win32/x86. Example: cmake -S . -B build/cmake -A Win32")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/third_party/ffmpeg" CACHE PATH "FFmpeg x86 shared development tree")
set(WINAMP_SDK "${CMAKE_SOURCE_DIR}/third_party/winamp-sdk" CACHE PATH "Directory containing in2.h and out.h")

set(PLUGIN_SOURCES
  src/audio_buffer.cpp
  src/ffmpeg_decoder.cpp
  src/ffmpeg_loader.cpp
  src/info_box.cpp
  src/log.cpp
  src/playback.cpp
  src/playlist_bypass.cpp
  src/playlist_file.cpp
  src/plugin.cpp
  src/settings.cpp
  src/url_util.cpp
  src/in_hls.rc
)

add_library(in_hls SHARED ${PLUGIN_SOURCES} src/in_hls.def)
target_include_directories(in_hls PRIVATE src "${WINAMP_SDK}" "${FFMPEG_ROOT}/include")
target_compile_definitions(in_hls PRIVATE WIN32 _WINDOWS NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_link_directories(in_hls PRIVATE "${FFMPEG_ROOT}/lib")
target_link_libraries(in_hls PRIVATE avformat avcodec avutil swresample delayimp)
target_link_options(in_hls PRIVATE
  "/DELAYLOAD:avformat-63.dll"
  "/DELAYLOAD:avcodec-63.dll"
  "/DELAYLOAD:avutil-61.dll"
  "/DELAYLOAD:swresample-7.dll")
set_target_properties(in_hls PROPERTIES
  OUTPUT_NAME "in_hls"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/build/Win32/Release"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/build/Win32/Debug"
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(in_hls_tests
  tests/test_main.cpp
  src/audio_buffer.cpp
  src/ffmpeg_decoder.cpp
  src/ffmpeg_loader.cpp
  src/log.cpp
  src/playback.cpp
  src/playlist_file.cpp
  src/settings.cpp
  src/url_util.cpp)
target_include_directories(in_hls_tests PRIVATE src "${WINAMP_SDK}" "${FFMPEG_ROOT}/include")
target_compile_definitions(in_hls_tests PRIVATE WIN32 _CONSOLE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_link_directories(in_hls_tests PRIVATE "${FFMPEG_ROOT}/lib")
target_link_libraries(in_hls_tests PRIVATE avformat avcodec avutil swresample delayimp)
target_link_options(in_hls_tests PRIVATE
  "/DELAYLOAD:avformat-63.dll"
  "/DELAYLOAD:avcodec-63.dll"
  "/DELAYLOAD:avutil-61.dll"
  "/DELAYLOAD:swresample-7.dll")
set_target_properties(in_hls_tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/build/Win32/Release"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/build/Win32/Debug"
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
