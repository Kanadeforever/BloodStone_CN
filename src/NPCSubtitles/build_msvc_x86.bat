@echo off
setlocal
chcp 65001 >nul

rem 重要：%%~dp0 自带末尾反斜杠。直接写成 "%%~dp0" 传给 CMake 时，  
rem Windows 参数解析可能把末尾反斜杠与收尾引号粘连，导致 -B/-A 被吞进 -S 参数。  
for %%I in ("%~dp0.") do set "ROOT=%%~fI"
set "BUILD=%ROOT%\..\..\build"

echo.
echo [信息] 版本：0.2.2_D3D9InteropSafe  
echo [信息] 源码目录：%ROOT%  
echo [信息] 构建目录：%BUILD%  

where cmake >nul 2>nul
if errorlevel 1 (
  echo [错误] 未找到 cmake.exe。  
  pause
  exit /b 1
)

if not exist "%BUILD%" mkdir "%BUILD%"
cmake -S "%ROOT%" -B "%BUILD%" -A Win32
if errorlevel 1 goto :fail
cmake --build "%BUILD%" --config Release
if errorlevel 1 goto :fail

echo.
echo [完成] 输出位于 build\Release\NPCSubtitles.asi  
pause
exit /b 0

:fail
echo.
echo [失败] 请保留完整控制台输出。  
pause
exit /b 1