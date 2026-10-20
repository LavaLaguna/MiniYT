@echo off
rem Quick build without opening Visual Studio: run this from "x64 Native Tools Command Prompt for VS".
cd /d "%~dp0src"
rc /nologo miniyt_msvc.rc
if errorlevel 1 goto :err
cl /nologo /std:c++17 /O2 /MT /EHsc /utf-8 /DUNICODE /D_UNICODE miniyt.cpp miniyt_msvc.res /Fe:..\MiniYT.exe /link /SUBSYSTEM:WINDOWS
if errorlevel 1 goto :err
del /q miniyt.obj miniyt_msvc.res 2>nul
echo.
echo Built: %~dp0MiniYT.exe
pause
exit /b 0
:err
echo BUILD FAILED
pause
exit /b 1
