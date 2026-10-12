@echo off
rem Compila el plugin (JPOGMouseFix.asi) y el proxy (dinput8.dll) en out\
rem Requiere Visual Studio con herramientas C++ (x86). Ajustar VCVARS si la ruta es otra.
setlocal
set "HERE=%~dp0"
if not defined VCVARS set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
call "%VCVARS%" >nul || exit /b 1
cd /d "%HERE%"
if not exist out mkdir out
if not exist obj mkdir obj
cl /nologo /O2 /MT /EHsc /GS- /std:c++17 /Foobj\ src\plugin\JPOGMouseFix.cpp /LD /link /OUT:out\JPOGMouseFix.asi /IMPLIB:obj\JPOGMouseFix.lib user32.lib kernel32.lib || exit /b 1
cl /nologo /O2 /MT /GS- /Foobj\ src\proxy\dinput8_proxy.cpp /LD /link /DEF:src\proxy\dinput8.def /OUT:out\dinput8.dll /IMPLIB:obj\dinput8.lib kernel32.lib || exit /b 1
copy /y config\JPOGMouseFix.ini out\ >nul
echo OK
