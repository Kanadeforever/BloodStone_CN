@echo off
chcp 65001 >nul
setlocal EnableExtensions
where py >nul 2>&1
if not errorlevel 1 (
    py -3 -B "%~dp0gui.pyw"
) else (
    python -B "%~dp0gui.pyw"
)
if errorlevel 1 (
    echo [错误] 请使用带 Tkinter 的 Python 3.10 或更高版本，并完整解压工具箱。  
    pause
    exit /b 1
)
exit /b 0
