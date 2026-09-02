@echo off
chcp 936 >nul
setlocal
for %%I in ("%~dp0.") do set "ROOT=%%~fI"
cd /d "%ROOT%"

if not exist "%ROOT%\.venv\Scripts\python.exe" (
    echo [准备] 首次运行，自动安装工具依赖...
    call "%ROOT%\安装依赖.bat" /auto
    if errorlevel 1 goto :fail
)
"%ROOT%\.venv\Scripts\python.exe" -c "import PIL, fontTools" >nul 2>nul
if errorlevel 1 (
    echo [准备] 依赖不完整，自动修复...
    call "%ROOT%\安装依赖.bat" /auto
    if errorlevel 1 goto :fail
)

echo ========================================
echo BloodstoneCN JSON 字符统计
echo ========================================
echo [1] 中文：扫描全部 JSON，严格排除日文字段
echo [2] 日文：只扫描明确的日文字段
set /p "MODE=请选择 1 或 2："
if "%MODE%"=="1" (
    set "LANG=zh"
    set "OUTNAME=中文"
) else if "%MODE%"=="2" (
    set "LANG=ja"
    set "OUTNAME=日文"
) else (
    echo [失败] 选择无效。
    goto :fail
)

echo.
set /p "JSONDIR=请输入 JSON 根目录（可把文件夹拖到窗口）："
set "JSONDIR=%JSONDIR:"=%"
if not exist "%JSONDIR%\" (
    echo [失败] 目录不存在：%JSONDIR%
    goto :fail
)

"%ROOT%\.venv\Scripts\python.exe" "%ROOT%\tools\JSON字符统计工具.py" "%JSONDIR%" --language %LANG% --output "%ROOT%\输出\字符统计_%OUTNAME%"
if errorlevel 1 goto :fail

echo.
echo [完成] 输出：%ROOT%\输出\字符统计_%OUTNAME%
pause
exit /b 0

:fail
echo.
echo [失败] 请保留以上控制台输出。
pause
exit /b 1
