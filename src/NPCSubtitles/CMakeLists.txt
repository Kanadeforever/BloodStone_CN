cmake_minimum_required(VERSION 3.20)
project(NPCSubtitles LANGUAGES CXX)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "NPCSubtitles targets 32-bit Bond.exe. Configure with -A Win32.")
endif()
add_library(NPCSubtitles SHARED
  src/common.cpp src/config.cpp src/log.cpp src/font_atlas.cpp src/resource_mode.cpp
  src/dialogue_index.cpp src/english_runtime_resolver.cpp src/native_font_capture.cpp
  src/renderer.cpp src/dialogue_hook.cpp src/plugin.cpp)
target_compile_features(NPCSubtitles PRIVATE cxx_std_17)
target_compile_definitions(NPCSubtitles PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
  target_compile_options(NPCSubtitles PRIVATE /W4 /permissive- /EHsc)
  set_property(TARGET NPCSubtitles PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set_target_properties(NPCSubtitles PROPERTIES PREFIX "" SUFFIX ".asi" OUTPUT_NAME "NPCSubtitles")
