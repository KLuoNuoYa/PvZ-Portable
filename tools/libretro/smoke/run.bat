@echo off
rem Re-run the core smoke test (game-data resolution + core bring-up).
rem Builds tools\libretro\smoke\smoke.exe, then drives the core through the
rem libretro API four times: BIOS mode, content mode, and two expected-failure
rem cases.
rem
rem Usage: tools\libretro\smoke\run.bat
rem
rem Game data: read from <root>\dist\system by default (put main.pak and the
rem properties folder in dist\system\pvz); set PVZ_SYSTEM_DIR to use a system
rem directory somewhere else, e.g. the one your frontend already has.

setlocal

call "%~dp0..\_find_toolchain.bat" || exit /b 1
set "PATH=%MSYS%\bin;%PATH%"

pushd "%~dp0"

set "BUILD=%~dp0..\..\..\build-libretro"
if not exist "%BUILD%\pvz_libretro.dll" (
    echo [error] build the core first: tools\libretro\build_libretro.bat
    popd & exit /b 1
)
copy /y "%BUILD%\pvz_libretro.dll" "pvz_libretro.dll" >nul

gcc -O1 -std=c11 -Wall smoke.c -o smoke.exe -I"%~dp0..\..\..\src\SexyAppFramework\platform\libretro"
if errorlevel 1 ( echo [error] smoke test build failed & popd & exit /b 1 )

rem %~dp0 ends with a backslash, which would escape the closing quote; use "%~dp0."
rem The game data is the one thing that cannot be committed (it is the retail
rem game's), so it is read from dist\system\pvz or from PVZ_SYSTEM_DIR.
if not defined PVZ_SYSTEM_DIR set "PVZ_SYSTEM_DIR=%~dp0..\..\..\dist\system"
smoke.exe pvz_libretro.dll "%PVZ_SYSTEM_DIR%" "%PVZ_SYSTEM_DIR%\pvz\main.pak" "%~dp0."
set "RC=%ERRORLEVEL%"

del /q pvz_libretro.dll 2>nul
rmdir /s /q empty-system 2>nul
del /q notagame.pak 2>nul
rmdir /s /q smoke-saves 2>nul

popd
exit /b %RC%
