@echo off
setlocal
if not defined VCVARS set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
call "%VCVARS%" >nul || exit /b 1
cd /d "%~dp0.."
cl /nologo /O2 /MT /EHsc /std:c++17 tools\watercheck.cpp /Foobj\watercheck.obj /Fe:out\watercheck.exe /link user32.lib || exit /b 1
out\watercheck.exe "%~1" "%cd%\out\d3d8.dll" 11 8
exit /b %errorlevel%
