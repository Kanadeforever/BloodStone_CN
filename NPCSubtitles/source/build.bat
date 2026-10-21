@echo off
chcp 65001 >nul
setlocal EnableExtensions
REM 默认构建完整 NPC 字幕候选：对白、原生字形和 D3D9 均启用。  
set "ROOT=%~dp0..\.."
set "BUILD=%ROOT%\.build\NPCSubtitles"
if not exist "%BUILD%" mkdir "%BUILD%"
REM 优先使用 Clang/LLD，构建出的 PE 使用可复现时间戳。  
where clang-cl >nul 2>&1
if not errorlevel 1 (
    where lld-link >nul 2>&1
    if not errorlevel 1 (
        where python >nul 2>&1
        if not errorlevel 1 (
            python "%~dp0tools\build_clang_cross.py"
            if errorlevel 1 exit /b 1
            echo [完成] 已编译完整功能 Win32 ASI。  
            exit /b 0
        )
    )
)
REM 无 Clang 时，使用用户当前 Visual Studio x86 命令提示符中的 MSVC。  
where cl >nul 2>&1
if errorlevel 1 (
    echo [错误] 需要 Clang-CL+LLD+Python，或 Visual Studio x86 开发命令行。  
    exit /b 1
)
where link >nul 2>&1
if errorlevel 1 exit /b 1
call :BuildStage 4
if errorlevel 1 exit /b 1
if exist "%BUILD%" rmdir /s /q "%BUILD%"
echo [完成] 已编译完整功能 Win32 ASI。  
exit /b 0
:BuildStage
set "STAGE=%~1"
set "OUTPUT=%ROOT%\release"
if "%STAGE%"=="2" set "OUTPUT=%ROOT%\release\阶段B_对白事件"
if "%STAGE%"=="3" set "OUTPUT=%ROOT%\release\阶段C_字体捕获"
if not exist "%OUTPUT%" mkdir "%OUTPUT%"
cl /nologo /c /W3 /O2 /GS- /GR- /Zl /EHsc- /DNPC_DIAG_STAGE=%STAGE% /Fo"%BUILD%\npc_%STAGE%.obj" "%~dp0src\native_standalone.cpp"
if errorlevel 1 exit /b 1
link /nologo /DLL /MACHINE:X86 /Brepro /ENTRY:DllMain@12 /NODEFAULTLIB /OUT:"%OUTPUT%\NPCSubtitles.asi" /IMPLIB:"%BUILD%\npc_%STAGE%.lib" "%BUILD%\npc_%STAGE%.obj" kernel32.lib
if errorlevel 1 exit /b 1
exit /b 0
