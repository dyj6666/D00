@echo off
rem D00 配套命令行终端启动器（双击即可）
cd /d "%~dp0"
set "PY=D:\Python\python.exe"
if exist "%PY%" (
    "%PY%" d00term.py %*
) else (
    python d00term.py %*
)
if errorlevel 1 pause
