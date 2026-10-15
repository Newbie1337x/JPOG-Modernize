@echo off
rem Builds the plugin (JPOGMouseFix.asi) and the proxy (dinput8.dll) into out\
rem Requires Visual Studio with C++ tools (x86). Set VCVARS if your install path differs.
setlocal
set "HERE=%~dp0"
if not defined VCVARS set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
call "%VCVARS%" >nul || exit /b 1
cd /d "%HERE%"
if not exist out mkdir out
if not exist obj mkdir obj
cl /nologo /O2 /MT /EHsc /GS- /std:c++17 /Foobj\ src\plugin\JPOGMouseFix.cpp /LD /link /OUT:out\JPOGMouseFix.asi /IMPLIB:obj\JPOGMouseFix.lib user32.lib kernel32.lib || exit /b 1
cl /nologo /O2 /MT /GS- /Foobj\ src\proxy\dinput8_proxy.cpp /LD /link /DEF:src\proxy\dinput8.def /OUT:out\dinput8.dll /IMPLIB:obj\dinput8.lib kernel32.lib || exit /b 1
cl /nologo /O2 /MD /EHsc /GS- /std:c++17 /W3 /Zi /Fdobj\d3d8_compile.pdb /Ithird_party\imgui /Ithird_party\fsr31\include /Foobj\ src\d3d8to11\*.cpp third_party\fsr31\backend\ffx_dx11.cpp third_party\imgui\imgui.cpp third_party\imgui\imgui_draw.cpp third_party\imgui\imgui_tables.cpp third_party\imgui\imgui_widgets.cpp third_party\imgui\backends\imgui_impl_dx11.cpp /LD /link /DEBUG /OPT:REF /OPT:ICF /PDB:out\d3d8.pdb /MAP:obj\d3d8.map /DEF:src\d3d8to11\d3d8.def /OUT:out\d3d8.dll /IMPLIB:obj\d3d8.lib third_party\fsr31\lib\ffx_backend_dx11_x86.lib third_party\fsr31\lib\ffx_fsr3upscaler_x86.lib third_party\fsr31\lib\ffx_frameinterpolation_x86.lib third_party\fsr31\lib\ffx_opticalflow_x86.lib d3d11.lib dxgi.lib user32.lib kernel32.lib || exit /b 1
copy /y config\JPOGMouseFix.ini out\ >nul
copy /y config\d3d8to11.ini out\ >nul
copy /y config\d3d8to11_shadervariants.bin out\ >nul
if not exist out\shaders\fsr1 mkdir out\shaders\fsr1
copy /y third_party\fsr1\ffx_a.h out\shaders\fsr1\ >nul
copy /y third_party\fsr1\ffx_fsr1.h out\shaders\fsr1\ >nul
copy /y third_party\fsr1\LICENSE.txt out\shaders\fsr1\ >nul
if not exist out\licenses\fsr31 mkdir out\licenses\fsr31
copy /y third_party\fsr31\LICENSE.txt out\licenses\fsr31\ >nul
copy /y third_party\fsr31\Third_party_notices.txt out\licenses\fsr31\ >nul
echo OK
