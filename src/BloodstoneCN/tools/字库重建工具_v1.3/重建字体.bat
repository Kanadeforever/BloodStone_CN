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

if not exist "%ROOT%\font_source\" mkdir "%ROOT%\font_source" >nul 2>nul

echo ========================================
echo BloodstoneCN Grid074 字体全量重建
echo ========================================
echo [1] 重建中文字体（排除 JSON 日文字段）
echo [2] 重建日文字体（只使用 JSON 日文字段）
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

echo.
echo [规则] 每次都根据当前 JSON 从零完整重建，不读取旧 fonts.bfa，不做增量追加。
echo [规则] 自动递归扫描 %ROOT%\font_source 中的 TTF/OTF/TTC/OTC。
echo [规则] 所选字体 cmap 中全部 Unicode 标点（P*）以及 U+3000 全角空格永久保留。
echo [规则] Grid074 固定为 2048x2048、36x44 单元、origin-x=2、baseline-y=35。
echo.
set "FONTNAME="
set /p "FONTNAME=可选：指定 font_source 内字体文件名/相对路径；直接回车则自动选择："
set "FONTNAME=%FONTNAME:"=%"

if defined FONTNAME (
    "%ROOT%\.venv\Scripts\python.exe" "%ROOT%\tools\font_atlas_tool.py" rebuild --language %LANG% --json-dir "%JSONDIR%" --font-source "%ROOT%\font_source" --font-name "%FONTNAME%" --output "%ROOT%\输出\%OUTNAME%"
) else (
    "%ROOT%\.venv\Scripts\python.exe" "%ROOT%\tools\font_atlas_tool.py" rebuild --language %LANG% --json-dir "%JSONDIR%" --font-source "%ROOT%\font_source" --output "%ROOT%\输出\%OUTNAME%"
)
if errorlevel 1 goto :fail

echo.
echo [完成] 最终字库：%ROOT%\输出\%OUTNAME%\fonts.bfa
pause
exit /b 0

:fail
echo.
echo [失败] 请保留以上控制台输出。
echo 如果提示 font_source 没有字体，请把字体文件放到：%ROOT%\font_source\
pause
exit /b 1
