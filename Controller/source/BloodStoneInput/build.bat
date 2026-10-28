@echo off
chcp 65001 >nul
setlocal EnableExtensions

REM 将工作目录切换到构建脚本所在位置，路径末尾加点以避免尾部反斜杠影响引号解析。  
pushd "%~dp0."
if errorlevel 1 goto :error
set "BSCN_PUSHED=1"
set "SRC=%CD%"
for %%I in ("%SRC%\..\..") do set "ROOT=%%~fI"
set "OUT=%ROOT%\release"
set "TMP=%ROOT%\.build\win32"

REM 默认使用 Visual Studio 2026 的 Win32 工程；其他 VS 环境可以通过变量覆盖生成器名称。  
if not defined BSCN_CMAKE_GENERATOR set "BSCN_CMAKE_GENERATOR=Visual Studio 18 2026"

REM 构建前检查源码入口和 CMake，避免错误路径产生难以理解的编译报错。  
if not exist "%SRC%\CMakeLists.txt" goto :error
where cmake >nul 2>nul
if errorlevel 1 goto :error
if not exist "%OUT%" mkdir "%OUT%"
if errorlevel 1 goto :error

REM SRC 由当前目录取得，不会以反斜杠结尾，确保 -S 与 -B 等参数分别传给 CMake。  
echo [配置] 源码目录："%SRC%"  
echo [配置] 构建目录："%TMP%"  
echo [配置] CMake 生成器：%BSCN_CMAKE_GENERATOR%  
cmake -S "%SRC%" -B "%TMP%" -G "%BSCN_CMAKE_GENERATOR%" -A Win32 -DJOYSHIM_BUILD_DIAGNOSTICS=OFF
if errorlevel 1 goto :error

REM 先单独检查图标翻译单元，避免其语法错误导致无关 SDL3 静态库反复编译。  
cmake --build "%TMP%" --config Release --target BondIconCore --parallel
if errorlevel 1 goto :error

REM 图标核心通过后，再构建包含输入与图标的唯一 dinput8 DLL。  
cmake --build "%TMP%" --config Release --target JoyShimDInput8 --parallel
if errorlevel 1 goto :error

REM VS 多配置生成器将 Release DLL 放在构建目录的 Release 子目录内。  
set "BUILT_DLL=%TMP%\Release\dinput8.dll"
if not exist "%BUILT_DLL%" goto :error
copy /Y "%BUILT_DLL%" "%OUT%\dinput8.dll" >nul
if errorlevel 1 goto :error

echo.
echo [完成] 单 DLL 已生成："%OUT%\dinput8.dll"  
if defined BSCN_PUSHED popd
pause
exit /b 0

:error
echo.
echo [失败] 请检查上方 CMake 或编译错误。  
if defined BSCN_PUSHED popd
pause
exit /b 1
