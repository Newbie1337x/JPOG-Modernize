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
cl /nologo /O2 /MT /EHsc /GS- /std:c++17 /W3 /Foobj\ src\d3d8to11\*.cpp /LD /link /DEF:src\d3d8to11\d3d8.def /OUT:out\d3d8.dll /IMPLIB:obj\d3d8.lib user32.lib kernel32.lib || exit /b 1
copy /y config\JPOGMouseFix.ini out\ >nul
copy /y config\d3d8to11.ini out\ >nul
echo OK
