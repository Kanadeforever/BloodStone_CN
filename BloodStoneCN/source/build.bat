@echo off
chcp 65001 >nul
setlocal EnableExtensions
set "ROOT=%~dp0.."
set "BUILD=%ROOT%\.build\BloodstoneCN"
set "OUT=%ROOT%\release\Runtime"
set "PYTHON_CMD=python"
where py >nul 2>&1
if not errorlevel 1 set "PYTHON_CMD=py -3"
if /i "%~1"=="--check-only" goto checks
if not exist "%BUILD%" mkdir "%BUILD%"
if not exist "%OUT%" mkdir "%OUT%"
where cl >nul 2>&1
if errorlevel 1 (
    echo [错误] 请在 Visual Studio Win32 开发命令提示符中执行。  
    exit /b 1
)
echo [构建] 编译 EN 原生 LOC 和 EUR 字体目录重定向插件。  
cl /nologo /c /W4 /WX /O2 /GS- /GR- /Zl /EHsc- /Fo"%BUILD%\bloodstone_cn.obj" "%~dp0src\bloodstone_cn.cpp"
if errorlevel 1 exit /b 1
link /nologo /Brepro /DLL /MACHINE:X86 /ENTRY:DllMain@12 /NODEFAULTLIB /IMPLIB:"%BUILD%\BloodstoneCN.lib" /OUT:"%OUT%\BloodstoneCN.asi" "%BUILD%\bloodstone_cn.obj" kernel32.lib
if errorlevel 1 exit /b 1
if exist "%BUILD%" rmdir /s /q "%BUILD%"
echo [完成] release\Runtime\BloodstoneCN.asi  
:checks
%PYTHON_CMD% -B "%~dp0tools\bootstrap_toolbox.py"
if errorlevel 1 exit /b 1
%PYTHON_CMD% -B "%~dp0tools\run_tests.py"
if errorlevel 1 exit /b 1
exit /b 0
