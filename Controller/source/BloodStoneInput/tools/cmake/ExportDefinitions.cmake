# 根据编译目标和工具链准备 DLL 的链接导出定义。
# 输入：source_def 是项目源代码内的原始 .def；mingw_def 是构建目录内的目标文件。
# 输出：通过 out_var 返回链接器实际读取的 .def 路径。
# 对于 MSVC 或 x64，原样使用源文件。只有 MinGW x86 需要转换。
function(joyshim_prepare_export_definition out_var source_def mingw_def)
    if(NOT EXISTS "${source_def}")
        message(FATAL_ERROR "找不到导出定义：${source_def}")
    endif()

    if(NOT MINGW OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(${out_var} "${source_def}" PARENT_SCOPE)
        return()
    endif()

    # 原始文件针对 MSVC 使用 _Function@参数字节数 的内部别名。
    # GNU PE 目标链接器会自动补一个下划线，因此其 .def 需要
    # Function@参数字节数 形式，避免链接时查找不存在的双下划线符号。
    # 只修改等号右边的内部别名，不修改左边的外部名字、序号或附加标志。
    file(READ "${source_def}" source_text)
    string(REGEX MATCHALL "=_[A-Za-z][A-Za-z0-9_]*@[0-9]+" decorated_aliases "${source_text}")
    if(NOT decorated_aliases)
        message(FATAL_ERROR "MinGW x86 导出表没有可转换的 stdcall 别名：${source_def}")
    endif()
    string(REGEX REPLACE "=_([A-Za-z][A-Za-z0-9_]*@[0-9]+)" "=\\1"
        mingw_text "${source_text}")
    if(mingw_text STREQUAL source_text)
        message(FATAL_ERROR "MinGW x86 导出表转换失败：${source_def}")
    endif()
    get_filename_component(output_dir "${mingw_def}" DIRECTORY)
    file(MAKE_DIRECTORY "${output_dir}")
    file(WRITE "${mingw_def}" "${mingw_text}")
    set(${out_var} "${mingw_def}" PARENT_SCOPE)
endfunction()
