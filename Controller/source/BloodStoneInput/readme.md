# Blood Stone 单 DLL 输入与图标插件：构建说明

## 一键构建

在 Windows x86 目标上使用 Visual Studio 2026（带 C++ 桌面工作负载）与 CMake，运行 `build.bat`。脚本会首先执行 `chcp 65001`，再使用规范化的源码目录作为 CMake `-S` 参数，避免带末尾反斜杠的引号被错误解析。

默认显式指定 `Visual Studio 18 2026` 生成器和 `Win32` 平台。如果开发环境使用其他兼容的 Visual Studio 生成器，可在启动脚本前设置 `BSCN_CMAKE_GENERATOR` 环境变量（例如 `Visual Studio 17 2022`）；不支持把 Ninja 生成器直接用于该脚本的 `-A Win32` 参数。
脚本只编译 `JoyShimDInput8`，发行产物只写入 `release/dinput8.dll`。

构建内嵌 SDL3 3.4.18 官方源码，不联网拉取 SDL，不依赖游戏目录另放 SDL3.dll。

## 部署

把编译得到的 **单一 `dinput8.dll`** 放在 `Bond.exe` 同目录。首次启动会覆盖原有 `dinput8.log`，之后追加本次进程日志。
不要同时部署旧版 `BondGamepadIcons.asi` 或另一份 JoyShim 代理，以免对同一 D3D9/XInput 路径重复 Hook。
无需放置任何 INI、TOML 或 `gamecontrollerdb.txt`；使用 SDL 官方内置映射数据库。

## 命令行

`Bond.exe -ps` 强制 PlayStation 图标；`-ns` 强制 Nintendo；`-xb` 强制 Xbox；`-auto` 恢复 SDL 自动识别。
参数不区分大小写，最后一个有效参数生效。


## 源码自检

如需在正式编译前单独检查条件编译和 XInput 头文件兼容性，可执行：

```bat
python tools\验证单DLL编译边界.py
```

无需 Windows SDK 即可完成基本结构检查；安装了 Clang 时，还会执行旧版、MinGW 和 MSVC 三组 XInput 头桩的语法检查。该自检不能代替 Windows 下的 `build.bat` 完整构建。

## 验证限制

本文件包目前仅完成源码层集成与基础静态检查，尚未经过 Windows 原生编译及游戏实机验收。
请勿将源码阶段的检测当成已完成的可运行 DLL。

为便于在大量 SDL3 源码参与构建前发现图标模块的语法错误，`build.bat` 会先编译 `BondIconCore`，通过后再编译最终唯一的 `JoyShimDInput8`。两次构建共用同一 `.build\win32` 中间目录，不会重复生成正式 DLL。
