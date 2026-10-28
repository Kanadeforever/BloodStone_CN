# 根据实际编译器选择 C++ std::string 移动构造的兼容保护。
# GCC 16 的某些 MinGW-w64 构建在 C++20 配合 -Os 时可能生成
# 无法由标准库满足的移动构造符号。项目正式目标固定 C++17，
# 此外保留有针对性的优化器回退，避免编译参数演变再次触发该缺陷。
function(joyshim_gcc_string_needs_fallback output)
    # 只针对 Windows 的 MinGW GCC 16；其他编译器不需要这个特殊开关。
    # x86 与 x64 共用规则，防止同一个 GCC 问题影响第二套 DLL 构建。
    if(MINGW
            AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
            AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "16.0"
            AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "17.0")
        set(${output} TRUE PARENT_SCOPE)
    else()
        set(${output} FALSE PARENT_SCOPE)
    endif()
endfunction()
