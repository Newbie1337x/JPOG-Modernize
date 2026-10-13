@echo off
setlocal
if not defined VCVARS set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
call "%VCVARS%" >nul || exit /b 1
cd /d "%~dp0.."
cl /nologo /O2 /MD /EHsc /std:c++17 tools\shadertest\shadertest.cpp obj\shaders.obj obj\common.obj /Foobj\shadertest.obj /Fe:out\shadertest.exe /link user32.lib kernel32.lib || exit /b 1
out\shadertest.exe "%~1"
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /EHsc /std:c++17 tools\watercheck.cpp /Foobj\watercheck.obj /Fe:out\watercheck.exe /link user32.lib || exit /b 1
for %%M in (20 21 22 23 24 25) do (
    out\watercheck.exe "%~1" "%cd%\out\d3d8.dll" %%M 8
    if errorlevel 1 exit /b 1
)
exit /b 0
