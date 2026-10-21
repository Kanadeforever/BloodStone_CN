@echo off
chcp 65001 >nul
setlocal EnableExtensions DisableDelayedExpansion
REM 始终以当前 BAT 所在目录为根目录，避免从其他工作目录启动时找错文件。  
set "ROOT=%~dp0"
set "TARGET=%ROOT%放入Bond.exe"
set "PYDIR=%ROOT%runtime\python"
set "PYTHON=%PYDIR%\python.exe"
set "INPUT=%TARGET%\Bond.exe"
set "OUTPUT=%TARGET%\Bond.asi-auto.exe"
set "RESULT=1"

echo ==============================================  
echo BloodStoneCN ASI 三目录保底加载补丁  
echo ==============================================  
echo.

REM 必须使用完整解压的官方便携运行时，严禁在线下载或调用系统 Python。  
if not exist "%PYTHON%" goto missing_runtime
if not exist "%PYDIR%\python312.dll" goto missing_runtime
if not exist "%PYDIR%\python312.zip" goto missing_runtime
if not exist "%PYDIR%\python312._pth" goto missing_runtime
if not exist "%ROOT%pe_asi_patch.py" (
    echo [错误] 缺少补丁脚本 pe_asi_patch.py。  
    goto finished
)

REM 先实际启动解释器检查版本，防止只有 python.exe 或误放其他大版本导致黑箱失败。  
"%PYTHON%" -I -X utf8 -c "import sys; print('[运行时] Python ' + sys.version.split()[0]); sys.exit(0 if sys.version_info[:2] == (3, 12) else 1)"
if errorlevel 1 (
    echo [错误] 便携 Python 无法正常启动，或版本不是 Python 3.12。  
    goto finished
)
REM 输入只认指定文件夹的 Bond.exe；输出使用不同名称，保护原始游戏程序。  
if not exist "%INPUT%" (
    echo [错误] 请把脱壳 Bond.exe 放到“放入Bond.exe”文件夹。  
    goto finished
)
if exist "%OUTPUT%" (
    echo [保护] 已经存在 Bond.asi-auto.exe，不会覆盖。  
    echo [提示] 请先备份或移走旧的补丁输出文件。  
    goto finished
)

echo [运行] 正在生成带三目录 ASI 自动加载入口的新 EXE。  
REM 使用隔离模式运行补丁，不读取用户 Python 包目录和环境变量。  
"%PYTHON%" -I -X utf8 "%ROOT%pe_asi_patch.py" "%INPUT%" --output "%OUTPUT%"
if errorlevel 1 (
    echo [失败] 补丁没有成功完成，请检查上方信息。  
    goto finished
)
set "RESULT=0"
echo.
echo [完成] 已生成 Bond.asi-auto.exe；原始 Bond.exe 未修改。  
echo [使用] 备份游戏 EXE 后，将输出复制回游戏目录并改名为 Bond.exe。  
echo [加载] scripts、CN_Data、plugins 内直属的全部 ASI。  
goto finished

:missing_runtime
echo [缺少运行时] 未找到完整的官方 Python 3.12 Windows 嵌入版。  
echo [放置位置] "%PYDIR%"  
echo [处理方法] 将官方 python-3.12.10-embed-win32.zip 中的所有文件  
echo            直接解压到上述目录，不要创建第二层文件夹。  
echo [离线说明] 本程序不会下载任何文件，也不使用系统 Python。  

:finished
REM 保留 CMD 上的诊断输出直到用户按键，再打开输入与输出文件夹。  
echo.
echo [结束] 按任意键关闭 CMD 窗口，并打开“放入Bond.exe”文件夹。  
pause >nul
start "" explorer.exe "%TARGET%"
exit /b %RESULT%
