@echo off
setlocal
if not defined VCVARS set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
call "%VCVARS%" >nul || exit /b 1
cd /d "%~dp0.."
cl /nologo /O2 /MT /EHsc /std:c++17 tools\watercheck.cpp /Foobj\watercheck.obj /Fe:out\watercheck.exe /link user32.lib || exit /b 1
out\watercheck.exe "%~1" "%cd%\out\d3d8.dll" 18 1
if errorlevel 1 exit /b 1
findstr /c:"Generated frames are being presented" out\d3d8to11.log >nul || exit /b 1
findstr /c:"FrameGen initialization/dispatch failed" /c:"Generated Present failed" /c:"FSR31 dispatch error" out\d3d8to11.log >nul && exit /b 1
out\watercheck.exe "%~1" "%cd%\out\d3d8.dll" 19 1
if errorlevel 1 exit /b 1
findstr /c:"FrameGen optical flow + interpolation dispatched" out\d3d8to11.log >nul || exit /b 1
findstr /c:"FrameGen initialization/dispatch failed" out\d3d8to11.log >nul && exit /b 1
exit /b 0
