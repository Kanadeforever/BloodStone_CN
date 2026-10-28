cmake_minimum_required(VERSION 3.21)
project(JoyShim VERSION 0.5.2 LANGUAGES C CXX)  # 每个目标必须分别为 x86 与 x64 构建

# 编译器默认的 C++ 版本可能高于项目要求。GCC 16 默认使用 C++20，
# 即使目标写了 cxx_std_17，也只代表“至少 C++17”，不会强制降级。
# 固定使用标准 C++17，保证构建环境不同也不会意外切换标准库实现路径。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MinGW GCC 16 在 C++20 + -Os 的部分构造函数优化中有已知链接缺陷。
# 已知有效的规避方式是禁止这一个优化，不必关掉其余体积优化。
# 把判断放在独立模块中，方便单元测试模拟各种编译器与版本。
include("${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/GccStringPolicy.cmake")
joyshim_gcc_string_needs_fallback(JOYSHIM_GCC_STRING_FALLBACK)
if(JOYSHIM_GCC_STRING_FALLBACK)
    message(STATUS "JoyShim C++ ABI：固定 C++17，并对 MinGW GCC 16 禁用构造函数克隆优化")
endif()

# 在耗时的 SDL3 编译开始前，用实际编译器链接一个最小的 std::string 移动
# 构造程序。这样可以提前拦截 GCC/libstdc++ ABI 不兼容，不再到八个 DLL
# 最后链接时才看到同一个错误。try-compile 只生成临时文件，不发布 EXE。
if(JOYSHIM_GCC_STRING_FALLBACK)
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-Os -std=c++17 -fno-declone-ctor-dtor")
    set(CMAKE_REQUIRED_LINK_OPTIONS "-static;-static-libgcc;-static-libstdc++")
    check_cxx_source_compiles([=[
        #include <string>
        #include <utility>
        int main() {
            std::string first{"JoyShim"};
            std::string second{std::move(first)};
            return second.size() != 7;
        }
    ]=] JOYSHIM_GCC_STRING_LINK_OK)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(NOT JOYSHIM_GCC_STRING_LINK_OK)
        message(FATAL_ERROR "MinGW GCC 16 的 C++ 标准库移动构造链接预检失败；请检查编译器和静态运行库是否匹配")
    endif()
endif()

# 正式构建只生成七个代理 DLL。测试与诊断程序均是开发工具，
# 不应该随着默认构建额外生成 EXE。开发人员可以显式打开相应开关。
option(BUILD_TESTING "启用 CTest 单元测试（开发专用）" OFF)
option(JOYSHIM_BUILD_DIAGNOSTICS "编译手柄与代理诊断工具（开发专用）" OFF)
include(CTest)

# 诊断构建选项。打开后 DllMain 不创建线程，改为首次真实代理 API
# 调用时才排队启动。这可隔离部分老游戏对 loader lock 启动线程的敏感性，
# 代价是若 winmm 代理从未被调用，就不能进行被动 IAT 接管。
option(JOYSHIM_LAZY_BOOTSTRAP_ONLY "DllMain 不创建线程，仅在真实 API 调用时启动" OFF)

# 所有目标统一采用 C++17。MSVC 必须显式使用 UTF-8 读取源文件，
# 否则中文注释可能在简体中文系统代码页下被误解析为非法 C++ 标记。
function(joyshim_configure_target target)
    target_compile_features(${target} PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /WX
            /permissive-
            /GR-
            /utf-8
        )
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Werror
            -fno-exceptions
            -fno-rtti
        )
        if(JOYSHIM_GCC_STRING_FALLBACK)
            # 仅对 GCC 16 的 C++ 目标关闭已知有缺陷的构造函数克隆优化。
            # 不影响 SDL3 的 C 驱动，也不影响 GCC 15、MSVC 与 Clang。
            target_compile_options(${target} PRIVATE -fno-declone-ctor-dtor)
        endif()
    endif()
endfunction()

if(BUILD_TESTING)
add_executable(test_mapping tests/test_mapping.cpp)
target_include_directories(test_mapping PRIVATE src)
joyshim_configure_target(test_mapping)
add_test(NAME mapping COMMAND test_mapping)

add_executable(test_slots tests/test_slots.cpp)
target_include_directories(test_slots PRIVATE src)
joyshim_configure_target(test_slots)
add_test(NAME slots COMMAND test_slots)

add_executable(test_device_correction tests/test_device_correction.cpp)
target_include_directories(test_device_correction PRIVATE src)
joyshim_configure_target(test_device_correction)
add_test(NAME device_correction COMMAND test_device_correction)

add_executable(test_input_routing tests/test_input_routing.cpp src/input_routing.cpp)
target_include_directories(test_input_routing PRIVATE src)
joyshim_configure_target(test_input_routing)
add_test(NAME input_routing COMMAND test_input_routing)

add_executable(test_toml_reader tests/test_toml_reader.cpp src/toml_reader.cpp)
target_include_directories(test_toml_reader PRIVATE src)
joyshim_configure_target(test_toml_reader)
add_test(NAME toml_reader COMMAND test_toml_reader)

add_executable(test_controller_match tests/test_controller_match.cpp src/controller_match.cpp)
target_include_directories(test_controller_match PRIVATE src)
joyshim_configure_target(test_controller_match)
add_test(NAME controller_match COMMAND test_controller_match)

add_executable(test_managed_device_scope tests/test_managed_device_scope.cpp)
target_include_directories(test_managed_device_scope PRIVATE src)
joyshim_configure_target(test_managed_device_scope)
add_test(NAME managed_device_scope COMMAND test_managed_device_scope)

add_executable(test_directinput_device_scope tests/test_directinput_device_scope.cpp)
target_include_directories(test_directinput_device_scope PRIVATE src)
joyshim_configure_target(test_directinput_device_scope)
add_test(NAME directinput_device_scope COMMAND test_directinput_device_scope)
endif()  # BUILD_TESTING

if(NOT WIN32)
    message(STATUS "非 Windows 环境：仅构建可移植单元测试。")
    return()
endif()

# Release 模式只删除链接器确认未使用的函数/数据，不手工裁剪 SDL3。
# GNU/LLVM 需要先把函数拆成独立段，否则静态库中的一个被引用函数
# 可能连带保留同一对象文件中的无关功能。MSVC 使用对应的链接器优化。
if(MSVC)
    add_link_options($<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
else()
    add_compile_options($<$<CONFIG:Release>:-ffunction-sections>
                        $<$<CONFIG:Release>:-fdata-sections>)
    add_link_options($<$<CONFIG:Release>:-Wl,--gc-sections>)
endif()

# MSVC 的 CRT 选项必须在添加 SDL3 子项目之前统一设置，
# 使 SDL3-static 与代理核心使用相同的 /MT 运行库，避免 /MD 与 /MT 混用
# 或使游戏目录还需要额外安装 Visual C++ 运行库。
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# 正式 Windows 发行版必须将 SDL3 静态链接进每个代理 DLL。
# 源码固定放在 tools/vendor/SDL3，禁止默认联网拉取动态 DLL 来凑数。
# 不通过手工删源文件裁剪，而是关闭不需要的子系统和构建产物。
set(JOYSHIM_SDL_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/tools/vendor/SDL3" CACHE PATH "完整的 SDL3 上游源码目录")
if(NOT EXISTS "${JOYSHIM_SDL_SOURCE}/CMakeLists.txt")
    message(FATAL_ERROR "缺少 SDL3 完整源码：${JOYSHIM_SDL_SOURCE}。正式构建要求随工程提供上游 SDL3 源码，不允许回退到外置 SDL3.dll。")
endif()
# 固定内嵌 SDL3 版本，避免编译环境意外使用其他版本的源码。
# 不修改官方源码；只读取它的公共版本头核对 3.4.18。
set(JOYSHIM_SDL_VERSION "3.4.18")
set(JOYSHIM_SDL_VERSION_HEADER "${JOYSHIM_SDL_SOURCE}/include/SDL3/SDL_version.h")
if(NOT EXISTS "${JOYSHIM_SDL_VERSION_HEADER}")
    message(FATAL_ERROR "SDL3 源码缺少公共版本头：${JOYSHIM_SDL_VERSION_HEADER}")
endif()
foreach(part MAJOR MINOR MICRO)
    file(STRINGS "${JOYSHIM_SDL_VERSION_HEADER}" SDL_${part}_HEADER_LINE
        REGEX "^[ \t]*#[ \t]*define[ \t]+SDL_${part}_VERSION[ \t]+[0-9]+")
    if(NOT SDL_${part}_HEADER_LINE)
        message(FATAL_ERROR "SDL3 公共版本头没有 SDL_${part}_VERSION")
    endif()
    string(REGEX MATCH "[0-9]+$" JOYSHIM_SDL_${part} "${SDL_${part}_HEADER_LINE}")
endforeach()
set(JOYSHIM_SDL_ACTUAL_VERSION
    "${JOYSHIM_SDL_MAJOR}.${JOYSHIM_SDL_MINOR}.${JOYSHIM_SDL_MICRO}")
if(NOT JOYSHIM_SDL_ACTUAL_VERSION VERSION_EQUAL JOYSHIM_SDL_VERSION)
    message(FATAL_ERROR "SDL3 源码版本 ${JOYSHIM_SDL_ACTUAL_VERSION} 与要求版本 ${JOYSHIM_SDL_VERSION} 不一致")
endif()
message(STATUS "JoyShim 内嵌 SDL3 版本：${JOYSHIM_SDL_ACTUAL_VERSION}")
set(SDL_SHARED OFF CACHE BOOL "不构建独立 SDL3.dll" FORCE)
set(SDL_STATIC ON CACHE BOOL "构建供 JoyShim 链接的 SDL3 静态库" FORCE)
set(SDL_TESTS OFF CACHE BOOL "关闭 SDL 自测" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "关闭 SDL 示例" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "关闭 SDL 安装目标" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "不使用 SDL 音频" FORCE)
set(SDL_VIDEO OFF CACHE BOOL "不使用 SDL 视频" FORCE)
set(SDL_RENDER OFF CACHE BOOL "不使用 SDL 渲染" FORCE)
set(SDL_GPU OFF CACHE BOOL "不使用 SDL GPU" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "不使用 SDL 摄像头" FORCE)
# JoyShim 不展示操作系统对话框、托盘、供电状态，也不消费陀螺仪数据。
# 禁用这些可选 SDL 子系统可减少进入静态 DLL 的代码；手柄 HIDAPI、
# Raw Input、XInput、热插拔及独立振动功能不得被这些选项禁用。
set(SDL_DIALOG OFF CACHE BOOL "不使用 SDL 对话框" FORCE)
set(SDL_TRAY OFF CACHE BOOL "不使用 SDL 托盘" FORCE)
set(SDL_POWER OFF CACHE BOOL "不查询 SDL 供电状态" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "不使用传感器子系统" FORCE)
set(SDL_VIRTUAL_JOYSTICK OFF CACHE BOOL "不创建 SDL 虚拟手柄" FORCE)
set(SDL_JOYSTICK ON CACHE BOOL "保留 SDL 控制器" FORCE)
set(SDL_HIDAPI ON CACHE BOOL "保留 HIDAPI 输入驱动" FORCE)
set(SDL_HIDAPI_JOYSTICK ON CACHE BOOL "保留 HIDAPI 手柄驱动" FORCE)
set(SDL_HAPTIC ON CACHE BOOL "保留可用的设备振动支持" FORCE)
add_subdirectory("${JOYSHIM_SDL_SOURCE}" "${CMAKE_BINARY_DIR}/sdl3" EXCLUDE_FROM_ALL)

# 静态链接会让每一种 DLL 自带一份 SDL3，Release 应首先减少实际机器码。
# GCC/Clang 的 -Os 侧重小体积；保留所有设备驱动，由链接器删除未用代码。
# 对于 IPO/LTO，先让 CMake 验证两种语言和当前链接器是否支持，
# 不支持时回退普通链接，避免不同 MinGW / MSVC 环境直接构建失败。
option(JOYSHIM_OPTIMIZE_SIZE "优先缩小 Release 静态代理 DLL" ON)
option(JOYSHIM_ENABLE_LTO "在支持时对 Release 启用跨模块链接时优化" OFF)
# 小型 IPO 试编译通过，不代表 SDL3 和完整代理在链接阶段一定能通过。
# 将编译器、目标位数与版本判断独立出来，供跨平台回归测试直接调用。
include("${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/LtoPolicy.cmake")
joyshim_lto_needs_fallback(JOYSHIM_LTO_NEEDS_FALLBACK)
set(JOYSHIM_IPO_SUPPORTED OFF)
set(JOYSHIM_EFFECTIVE_LTO OFF)
if(JOYSHIM_OPTIMIZE_SIZE)
    if(MSVC)
        target_compile_options(SDL3-static PRIVATE $<$<CONFIG:Release>:/O1>)
    else()
        target_compile_options(SDL3-static PRIVATE $<$<CONFIG:Release>:-Os>)
    endif()
endif()
if(JOYSHIM_ENABLE_LTO)
    if(JOYSHIM_LTO_NEEDS_FALLBACK)
        # 已知工具链组合在完整 DLL 的 LTO 别名分析中可能发生编译器内部错误。
        # 在进入 SDL3/代理链接之前禁用 IPO，保留 -Os 与未使用代码消除。
        message(STATUS "JoyShim Release LTO：MinGW GCC 16.x 采用兼容性回退，x86/x64 禁用 LTO")
    else()
        include(CheckIPOSupported)
        check_ipo_supported(RESULT JOYSHIM_IPO_SUPPORTED OUTPUT JOYSHIM_IPO_REASON
            LANGUAGES C CXX)
        if(JOYSHIM_IPO_SUPPORTED)
            set(JOYSHIM_EFFECTIVE_LTO ON)
            message(STATUS "JoyShim Release LTO：启用")
        else()
            message(STATUS "JoyShim Release LTO：不支持，使用普通链接（${JOYSHIM_IPO_REASON}）")
        endif()
    endif()
else()
    message(STATUS "JoyShim Release LTO：已由构建配置关闭")
endif()
if(JOYSHIM_EFFECTIVE_LTO)
    set_property(TARGET SDL3-static PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
else()
    # 显式设为 OFF，避免构建环境的全局 IPO 默认值意外重新启用 LTO。
    set_property(TARGET SDL3-static PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE FALSE)
endif()
if(NOT TARGET SDL3::SDL3-static)
    message(FATAL_ERROR "SDL3 没有提供 SDL3::SDL3-static，不能形成单 DLL 发行版。")
endif()

# 配置作用域测试依赖 RuntimeConfig 中的 Windows 路径类型，因此只在
# Windows 双工具链中构建。测试不访问文件系统，只验证“当前游戏父集合 ->
# 匹配手柄子集合”的字段级继承和覆盖语义。
if(BUILD_TESTING)
# 本测试直接读取随包默认模板，验证严格解析器而不止是 TOML 词法解析。
# 它只能在 Windows 实际构建中运行，避免通过 Linux 假头文件伪造结果。
add_executable(test_config_file
    tests/test_config_file.cpp
    src/config.cpp
    src/controller_match.cpp
    src/toml_reader.cpp
    src/input_routing.cpp
)
target_include_directories(test_config_file PRIVATE src)
target_compile_definitions(test_config_file PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
joyshim_configure_target(test_config_file)
add_test(NAME config_file COMMAND test_config_file)

add_executable(test_config_scope
    tests/test_config_scope.cpp
    src/config.cpp
    src/controller_match.cpp
    src/toml_reader.cpp
    src/input_routing.cpp
)
target_include_directories(test_config_scope PRIVATE src)
target_compile_definitions(test_config_scope PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
)
joyshim_configure_target(test_config_scope)
add_test(NAME config_scope COMMAND test_config_scope)
endif()  # BUILD_TESTING

# 本工程是 Blood Stone 专用单 DLL，而不是可改名的通用代理合集。
# 游戏通过 DInput8 导入加载本 DLL，另由原有输入接管逻辑重定向 XInput；
# 不能混合 XInput 代理的固定序号导出，否则会与 DInput8 导出冲突。
# Blood Stone 专用版的 D3D9 Hook 使用固定 x86 地址，禁止编译 x64。
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "BloodStoneInput 仅支持 32 位 Bond.exe；必须构建 Win32/x86。")
endif()
set(JOYSHIM_CORE_SOURCES
    src/bond_one_log.cpp
    src/xinput_proxy.cpp
    src/config.cpp
    src/controller_match.cpp
    src/toml_reader.cpp
    src/input_routing.cpp
    src/mapping_database.cpp
    src/sdl_backend.cpp
    src/proxy_entry.cpp
    src/proxy_system.cpp
    src/runtime_start.hpp
    src/directinput_filter.cpp
    src/managed_devices.cpp
    src/system_exports.cpp
)

# 图标模块的裸汇编和游戏内存探针沿用独立的警告规则，避免 JoyShim 的
# /WX 或 MinGW -Werror 误把既有非致命告警升级为构建失败。
add_library(BondIconCore OBJECT src/gamepad_icons/BondGamepadIcons.cpp)
target_include_directories(BondIconCore PRIVATE src)
target_compile_features(BondIconCore PRIVATE cxx_std_17)
target_compile_definitions(BondIconCore PRIVATE
    BSCN_SINGLE_DLL=1 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
if(MSVC)
    target_compile_options(BondIconCore PRIVATE /W4 /permissive- /utf-8 /EHsc /Zc:threadSafeInit-)
else()
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "图标模块使用 Microsoft SEH 和 x86 裸汇编；MinGW 必须选择 LLVM-MinGW Clang。")
    endif()
    target_compile_options(BondIconCore PRIVATE -fms-extensions -Wall -Wextra)
endif()
set_target_properties(BondIconCore PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_property(TARGET BondIconCore PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)

add_library(JoyShimCore OBJECT ${JOYSHIM_CORE_SOURCES})
target_include_directories(JoyShimCore PRIVATE src)
target_compile_definitions(JoyShimCore PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    BSCN_SINGLE_DLL=1
    UNICODE
    _UNICODE
)
joyshim_configure_target(JoyShimCore)
if(MSVC)
    target_compile_options(JoyShimCore PRIVATE /wd4505)
endif()
if(MSVC)
    target_compile_options(JoyShimCore PRIVATE /Zc:threadSafeInit- /EHsc)
endif()
if(JOYSHIM_OPTIMIZE_SIZE)
    if(MSVC)
        target_compile_options(JoyShimCore PRIVATE $<$<CONFIG:Release>:/O1>)
    else()
        target_compile_options(JoyShimCore PRIVATE $<$<CONFIG:Release>:-Os>)
    endif()
endif()
# 运行时核心与 SDL3 必须使用同一 LTO 策略；不能只关一个目标。
set_property(TARGET JoyShimCore PROPERTY
    INTERPROCEDURAL_OPTIMIZATION_RELEASE ${JOYSHIM_EFFECTIVE_LTO})
target_compile_definitions(JoyShimCore PRIVATE JOYSHIM_SDL_STATIC=1)
target_link_libraries(JoyShimCore PRIVATE SDL3::SDL3-static)
if(JOYSHIM_LAZY_BOOTSTRAP_ONLY)
    target_compile_definitions(JoyShimCore PRIVATE JOYSHIM_LAZY_BOOTSTRAP_ONLY=1)
endif()
set_target_properties(JoyShimCore PROPERTIES POSITION_INDEPENDENT_CODE ON)

# 32 位 Windows 的 stdcall 内部符号在对象文件中带前导下划线。
# MSVC 的 .def 使用完整的 COFF 符号名称；MinGW 的 PE 链接器
# 会自行补前导下划线。生成工具链专用 .def，避免出现重复前缀。
# 公开导出名称、序号和 NONAME/PRIVATE 仍沿用同一套原始定义。
include("${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/ExportDefinitions.cmake")

set(JOYSHIM_PROXY_TARGETS)

function(joyshim_add_proxy target output_name export_id)
    add_library(${target} SHARED $<TARGET_OBJECTS:JoyShimCore> $<TARGET_OBJECTS:BondIconCore>)
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "${output_name}"
        PREFIX ""
        SUFFIX ".dll"
    )
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(export_definition "${CMAKE_CURRENT_SOURCE_DIR}/src/exports_${export_id}_x86.def")
    else()
        set(export_definition "${CMAKE_CURRENT_SOURCE_DIR}/src/exports_${export_id}_x64.def")
    endif()
    # MinGW x86 使用构建目录内的转换副本，绝不改动 MSVC 所需的原始导出表。
    joyshim_prepare_export_definition(selected_definition
        "${export_definition}"
        "${CMAKE_CURRENT_BINARY_DIR}/exports/${export_id}_x86_mingw.def")
    target_sources(${target} PRIVATE "${selected_definition}")
    target_link_libraries(${target} PRIVATE SDL3::SDL3-static d3d9 shell32)
    if(MSVC)
        target_link_options(${target} PRIVATE /SAFESEH:NO)
    endif()
    # 单个 DInput8 入口链接 SDL3、输入核心及图标核心。
    set_property(TARGET ${target} PROPERTY
        INTERPROCEDURAL_OPTIMIZATION_RELEASE ${JOYSHIM_EFFECTIVE_LTO})
    if(MSVC)
        target_link_options(${target} PRIVATE /WX)
    else()
        if(JOYSHIM_OPTIMIZE_SIZE)
            # 只裁剪链接完成后留下的调试/符号信息，不移除 PE 正式导出表。
            target_link_options(${target} PRIVATE $<$<CONFIG:Release>:-Wl,--strip-all>)
        endif()
    endif()
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # -static-libgcc/-static-libstdc++ 只覆盖 GCC 与 C++ 运行库，
        # 不保证静态选择 MinGW 的 libwinpthread.a。-static 让 GNU ld
        # 优先使用静态版 winpthread，系统 Windows DLL 仍通过导入库链接。
        # 发行检查会读取 PE 导入表，发现 libwinpthread-1.dll 等依赖直接失败。
        # LLVM-MinGW 使用不同的运行库，不传入这些 GCC 专属参数。
        target_link_options(${target} PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    set(JOYSHIM_PROXY_TARGETS ${JOYSHIM_PROXY_TARGETS} ${target} PARENT_SCOPE)
endfunction()

# 当前专版固定只创建一个代理 DLL，不发布其他可改名入口。
joyshim_add_proxy(JoyShimDInput8 "dinput8" dinput8)

if(JOYSHIM_BUILD_DIAGNOSTICS)
add_executable(JoyShimGamepadProbe tools/src/gamepad_probe.cpp src/config.cpp src/controller_match.cpp src/toml_reader.cpp src/input_routing.cpp src/mapping_database.cpp src/sdl_backend.cpp)
target_include_directories(JoyShimGamepadProbe PRIVATE src)
target_compile_definitions(JoyShimGamepadProbe PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
)
joyshim_configure_target(JoyShimGamepadProbe)
target_compile_definitions(JoyShimGamepadProbe PRIVATE JOYSHIM_SDL_STATIC=1)
target_link_libraries(JoyShimGamepadProbe PRIVATE SDL3::SDL3-static)

add_executable(JoyShimGamepadInfo tools/src/gamepad_info.cpp src/config.cpp src/controller_match.cpp src/toml_reader.cpp src/input_routing.cpp src/mapping_database.cpp src/sdl_backend.cpp)
target_include_directories(JoyShimGamepadInfo PRIVATE src)
target_compile_definitions(JoyShimGamepadInfo PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
)
joyshim_configure_target(JoyShimGamepadInfo)
target_compile_definitions(JoyShimGamepadInfo PRIVATE JOYSHIM_SDL_STATIC=1)
target_link_libraries(JoyShimGamepadInfo PRIVATE SDL3::SDL3-static)


add_executable(JoyShimConfigCheck tools/src/config_check.cpp src/config.cpp src/controller_match.cpp src/toml_reader.cpp src/input_routing.cpp)
target_include_directories(JoyShimConfigCheck PRIVATE src)
target_compile_definitions(JoyShimConfigCheck PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
)
joyshim_configure_target(JoyShimConfigCheck)

add_executable(JoyShimXInputStateProbe tools/src/xinput_state_probe.cpp)
target_include_directories(JoyShimXInputStateProbe PRIVATE src)
target_compile_definitions(JoyShimXInputStateProbe PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
)
joyshim_configure_target(JoyShimXInputStateProbe)

add_executable(JoyShimProxyEntryProbe tools/src/proxy_entry_probe.cpp)
target_include_directories(JoyShimProxyEntryProbe PRIVATE src)
target_compile_definitions(JoyShimProxyEntryProbe PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
)
joyshim_configure_target(JoyShimProxyEntryProbe)

# Windows 宽字符诊断工具使用 wmain 入口，能够保留 Unicode DLL 路径。
# MinGW-w64 默认的 CRT 启动入口面向 main，缺少 -municode 时会在链接
# 阶段报 WinMain@16 未定义；普通 main 的诊断工具不应使用该参数。
# MSVC 可直接识别 wmain，不能给它传 MinGW 专属的链接选项。
if(MINGW)
    foreach(target JoyShimGamepadProbe JoyShimGamepadInfo
                   JoyShimConfigCheck JoyShimProxyEntryProbe)
        target_link_options(${target} PRIVATE -municode)
    endforeach()
endif()

endif()  # JOYSHIM_BUILD_DIAGNOSTICS

if(MSVC)
    set_property(TARGET JoyShimCore PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

foreach(target ${JOYSHIM_PROXY_TARGETS})
    if(MSVC)
        # 静态链接 MSVC 运行库，减少游戏目录中的额外运行库依赖。
        set_property(TARGET ${target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endforeach()
