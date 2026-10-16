@echo off
rem Build MiniYT.exe with Visual Studio's compiler.
rem Open the Start menu, search "x64 Native Tools Command Prompt for VS", open it, then run this file from there.
cd /d "%~dp0"
rc /nologo miniyt_msvc.rc
if errorlevel 1 goto :err
cl /nologo /std:c++17 /O2 /MT /EHsc /utf-8 /DUNICODE /D_UNICODE miniyt.cpp miniyt_msvc.res /Fe:MiniYT.exe /link /SUBSYSTEM:WINDOWS
if errorlevel 1 goto :err
del /q miniyt.obj miniyt_msvc.res 2>nul
echo.
echo Built: %~dp0MiniYT.exe
pause
exit /b 0
:err
echo.
echo BUILD FAILED - copy the first few error lines and send them to me.
pause
exit /b 1
