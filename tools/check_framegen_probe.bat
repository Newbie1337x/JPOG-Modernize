@echo off
setlocal
if not defined VCVARS set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
call "%VCVARS%" >nul || exit /b 1
cd /d "%~dp0.."
cl /nologo /O2 /MD /EHsc /std:c++17 /Ithird_party\fsr31\include tools\framegen_probe.cpp third_party\fsr31\backend\ffx_dx11.cpp /Foobj\ /Fe:out\framegen_probe.exe /link third_party\fsr31\lib\ffx_backend_dx11_x86.lib third_party\fsr31\lib\ffx_frameinterpolation_x86.lib third_party\fsr31\lib\ffx_opticalflow_x86.lib d3d11.lib dxgi.lib || exit /b 1
out\framegen_probe.exe
exit /b %errorlevel%
