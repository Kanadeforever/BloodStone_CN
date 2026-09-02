cmake_minimum_required(VERSION 3.20)
project(BloodstoneCN_R10_4_W1_2b_CoverageRollbackPerfSafe LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "BloodstoneCN targets 32-bit Windows / Bond.exe")
endif()
if(CMAKE_SIZEOF_VOID_P AND NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Bond.exe is 32-bit. Configure MSVC with -A Win32.")
endif()

add_library(BloodstoneCN SHARED
  src/common.cpp
  src/config.cpp
  src/external_loc.cpp
  src/font_atlas.cpp
  src/hooks.cpp
  src/interop_host.cpp
  src/loc_text_bridge.cpp
  src/log.cpp
  src/native_font_bridge.cpp
  src/plugin.cpp
  src/submit_text_bridge.cpp
  src/text_coverage_resolver.cpp
)

target_compile_features(BloodstoneCN PRIVATE cxx_std_17)
target_compile_definitions(BloodstoneCN PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(BloodstoneCN PRIVATE d3d9)
set_target_properties(BloodstoneCN PROPERTIES PREFIX "" SUFFIX ".asi" OUTPUT_NAME "BloodstoneCN")

if(MSVC)
  set_property(TARGET BloodstoneCN PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
  target_compile_options(BloodstoneCN PRIVATE /O2 /Ob2 /Gy /Gw /W4 /permissive- /EHsc /utf-8 /GR- /Zc:threadSafeInit-)
  target_link_options(BloodstoneCN PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO /SAFESEH:NO)
else()
  message(FATAL_ERROR "本测试包先按 MSVC Win32 验证。")
endif()

add_custom_command(TARGET BloodstoneCN POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/config/BloodstoneCN.ini"
          "$<TARGET_FILE_DIR:BloodstoneCN>/BloodstoneCN.ini")
