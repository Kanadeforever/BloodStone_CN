@echo off
setlocal EnableExtensions
cd /d "%~dp0"
set "CMAKE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%CMAKE%" set "CMAKE=cmake"
if exist build\msvc-x86 rmdir /s /q build\msvc-x86
"%CMAKE%" -S . -B build\msvc-x86 -A Win32
if errorlevel 1 exit /b 1
"%CMAKE%" --build build\msvc-x86 --config Release
if errorlevel 1 exit /b 1
if not exist release mkdir release
copy /y "build\msvc-x86\Release\NPCSubtitles.asi" "release\NPCSubtitles.asi" >nul
copy /y "config\NPCSubtitles.ini" "release\NPCSubtitles.ini" >nul
copy /y "完整接档.md" "release\完整接档.md" >nul
copy /y "使用说明.md" "release\使用说明.md" >nul
echo.
echo Build complete: release\NPCSubtitles.asi
endlocal
