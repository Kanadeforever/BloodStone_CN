cmake_minimum_required(VERSION 3.20)
project(NPCSubtitles VERSION 0.1.0 LANGUAGES CXX)
# Windows MSVC 构建与 Clang/LLD 交叉构建运行完全相同的独立 Win32 实现。
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "请使用 Visual Studio 的 Win32/x86 编译环境；跨平台用 tools/build_clang_cross.py")
endif()
add_library(NPCSubtitles SHARED src/native_standalone.cpp)
# 默认发行包含 Dialogue、字形和 D3D9 Hook，阶段 1～3 仅作为独立诊断备选。
target_compile_definitions(NPCSubtitles PRIVATE NPC_DIAG_STAGE=4)
target_compile_features(NPCSubtitles PRIVATE cxx_std_17)
target_compile_options(NPCSubtitles PRIVATE /O2 /W3 /GS- /GR- /Zl /EHsc- /utf-8)
target_link_libraries(NPCSubtitles PRIVATE kernel32)
target_link_options(NPCSubtitles PRIVATE /NODEFAULTLIB /Brepro "/ENTRY:DllMain@12")
set_target_properties(NPCSubtitles PROPERTIES PREFIX "" SUFFIX ".asi" OUTPUT_NAME "NPCSubtitles")
add_custom_command(TARGET NPCSubtitles POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/../../release"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:NPCSubtitles>" "${CMAKE_CURRENT_SOURCE_DIR}/../../release/NPCSubtitles.asi")
