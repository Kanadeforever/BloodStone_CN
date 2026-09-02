@echo off
chcp 936 >nul
setlocal
for %%I in ("%~dp0.") do set "ROOT=%%~fI"
cd /d "%ROOT%"

set "AUTO=0"
if /i "%~1"=="/auto" set "AUTO=1"

echo ========================================
echo BloodstoneCN 字库工具依赖安装
echo ========================================
where py >nul 2>nul
if errorlevel 1 (
    echo [失败] 未找到 Python Launcher ^(py.exe^)。
    echo 请先安装 Python 3，并确保 py 命令可用。
    goto :fail
)

if not exist "%ROOT%\.venv\Scripts\python.exe" (
    echo [1/2] 创建本地 Python 虚拟环境 .venv ...
    py -3 -m venv "%ROOT%\.venv"
    if errorlevel 1 goto :fail
) else (
    echo [1/2] 已存在 .venv，继续使用。
)

echo [2/2] 安装/更新 Pillow 与 fontTools ...
"%ROOT%\.venv\Scripts\python.exe" -m pip install -r "%ROOT%\tools\requirements.txt"
if errorlevel 1 goto :fail

echo.
echo [完成] 工具依赖已就绪。
if "%AUTO%"=="0" pause
exit /b 0

:fail
echo.
echo [失败] 依赖环境没有准备完成，请保留以上控制台输出。
if "%AUTO%"=="0" pause
exit /b 1
