@echo off
rem Copies the build into the game folder. Usage: install.bat "C:\Games\Jurassic Park Operation Genesis"
rem The .ini is only copied when missing, so user settings are never overwritten.
setlocal
set "GAME=%~1"
if "%GAME%"=="" set "GAME=C:\Games\Jurassic Park Operation Genesis"
if not exist "%GAME%\SimJP.exe" (echo SimJP.exe not found in "%GAME%" & exit /b 1)
tasklist /fi "imagename eq SimJP.exe" | "%SystemRoot%\System32\find.exe" /i "SimJP.exe" >nul && (echo Close the game before installing & exit /b 1)
copy /y "%~dp0out\JPOGMouseFix.asi" "%GAME%\" >nul || exit /b 1
copy /y "%~dp0out\dinput8.dll" "%GAME%\" >nul || exit /b 1
if not exist "%GAME%\JPOGMouseFix.ini" copy "%~dp0out\JPOGMouseFix.ini" "%GAME%\" >nul
echo Installed to "%GAME%"
