@echo off
REM Launcher for the NucleoV2 video converter (Tkinter GUI).
setlocal
set "PY=%LOCALAPPDATA%\Programs\Python\Python312\python.exe"
if not exist "%PY%" set "PY=python"
start "" "%PY%" "%~dp0nucleo_video_converter.py"
