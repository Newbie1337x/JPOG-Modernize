@echo off
rem Copies the build into the game folder. Usage: install.bat "C:\Games\Jurassic Park Operation Genesis"
rem .ini files are only copied when missing, so user settings are never overwritten.
rem If an existing d3d8.dll is dgVoodoo, it is kept as D3D8_dgVoodoo.dll ([Renderer] Backend=dgvoodoo).
setlocal
set "GAME=%~1"
if "%GAME%"=="" set "GAME=C:\Games\Jurassic Park Operation Genesis"
if not exist "%GAME%\SimJP.exe" (echo SimJP.exe not found in "%GAME%" & exit /b 1)
tasklist /fi "imagename eq SimJP.exe" | "%SystemRoot%\System32\find.exe" /i "SimJP.exe" >nul && (echo Close the game before installing & exit /b 1)
copy /y "%~dp0out\JPOGMouseFix.asi" "%GAME%\" >nul || exit /b 1
copy /y "%~dp0out\dinput8.dll" "%GAME%\" >nul || exit /b 1
if not exist "%GAME%\JPOGMouseFix.ini" copy "%~dp0out\JPOGMouseFix.ini" "%GAME%\" >nul
if exist "%~dp0out\d3d8.dll" (
    if exist "%GAME%\d3d8.dll" if not exist "%GAME%\D3D8_dgVoodoo.dll" (
        "%SystemRoot%\System32\findstr.exe" /m /c:"dgVoodoo" "%GAME%\d3d8.dll" >nul && move "%GAME%\d3d8.dll" "%GAME%\D3D8_dgVoodoo.dll" >nul
    )
    copy /y "%~dp0out\d3d8.dll" "%GAME%\" >nul || exit /b 1
    if not exist "%GAME%\d3d8to11.ini" copy "%~dp0out\d3d8to11.ini" "%GAME%\" >nul
)
echo Installed to "%GAME%"
