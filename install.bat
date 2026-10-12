@echo off
rem Copia el build a la carpeta del juego. Uso: install.bat "C:\Games\Jurassic Park Operation Genesis"
rem El .ini solo se copia si no existe (para no pisar la configuracion del usuario).
setlocal
set "GAME=%~1"
if "%GAME%"=="" set "GAME=C:\Games\Jurassic Park Operation Genesis"
if not exist "%GAME%\SimJP.exe" (echo No se encontro SimJP.exe en "%GAME%" & exit /b 1)
tasklist /fi "imagename eq SimJP.exe" | "%SystemRoot%\System32\find.exe" /i "SimJP.exe" >nul && (echo Cerra el juego antes de instalar & exit /b 1)
copy /y "%~dp0out\JPOGMouseFix.asi" "%GAME%\" >nul || exit /b 1
copy /y "%~dp0out\dinput8.dll" "%GAME%\" >nul || exit /b 1
if not exist "%GAME%\JPOGMouseFix.ini" copy "%~dp0out\JPOGMouseFix.ini" "%GAME%\" >nul
echo Instalado en "%GAME%"
