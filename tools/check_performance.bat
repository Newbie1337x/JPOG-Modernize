@echo off
setlocal
if not defined VCVARS set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
call "%VCVARS%" >nul || exit /b 1
cd /d "%~dp0.."
cl /nologo /O2 /MT /EHsc /std:c++17 tools\performance_check.cpp src\d3d8to11\performance.cpp src\d3d8to11\common.cpp /Foobj\ /Fe:out\performance_check.exe /link user32.lib || exit /b 1
out\performance_check.exe
exit /b %errorlevel%
