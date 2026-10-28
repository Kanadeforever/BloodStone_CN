# 用于判断当前编译器组合是否需要禁用 LTO 的兼容性模块。
# 这里只根据 CMake 已识别的操作系统、架构和编译器版本决策，
# 不负责修改外部工具链，也不会对 SDL3 上游源码做任何更改。
function(joyshim_lto_needs_fallback output)
    # 部分 MinGW-w64 GCC 版本在整个 SDL3 和 C++ 标准库的跨模块别名
    # 分析中可能崩溃，简单的 IPO 探测无法覆盖这类大型链接。
    # x86 已经出现故障；相同 GCC 优化器也用于 x64，因此同时回退。
    # 其他 GNU 主版本、MSVC 和 Clang 仍由正常的 IPO 支持检测决定。
    if(MINGW
            AND CMAKE_C_COMPILER_ID STREQUAL "GNU"
            AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
            AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "16.0"
            AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "17.0")
        set(${output} TRUE PARENT_SCOPE)
    else()
        set(${output} FALSE PARENT_SCOPE)
    endif()
endfunction()
