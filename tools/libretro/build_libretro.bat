@echo off
rem ---------------------------------------------------------------------------
rem Build the Plants vs. Zombies libretro core (pvz_libretro.dll).
rem
rem Toolchain:
rem   * MinGW-w64 gcc/g++ from MSYS2   (provides SDL2, zlib, libpng, libjpeg)
rem   * CMake shipped with Visual Studio
rem
rem Paths are auto-detected from the usual install locations.  Override any of
rem them with an environment variable, e.g.
rem     set MSYS=C:\msys64\ucrt64
rem     set VSCMAKE=C:\Program Files\Microsoft Visual Studio\2022\Community\...\CMake\bin
rem
rem Usage:
rem   tools\libretro\build_libretro.bat [Release|Debug] [--openmpt] [--clean]
rem
rem   --openmpt  link libopenmpt so MO3 background music plays
rem              (needs third_party\libopenmpt, see tools\libretro\build_libopenmpt.bat)
rem
rem Everything is relative to the repository root; the build tree, the staged
rem core and the local dependency copies are all gitignored.
rem ---------------------------------------------------------------------------
setlocal EnableDelayedExpansion

rem %~dp0 ends with a backslash; this script lives in <root>\tools\libretro, and
rem the repository root is both two levels up and the CMake source directory.
pushd "%~dp0..\.."
set "ROOT=%CD%"
popd

call "%~dp0_find_toolchain.bat" || exit /b 1

set "BUILD_TYPE=Release"
set "OPENMPT=OFF"
set "CLEAN=0"

:parse
if "%~1"=="" goto done_parse
if /I "%~1"=="Debug"     set "BUILD_TYPE=Debug"
if /I "%~1"=="Release"   set "BUILD_TYPE=Release"
if /I "%~1"=="--openmpt" set "OPENMPT=ON"
if /I "%~1"=="--clean"   set "CLEAN=1"
shift
goto parse
:done_parse

if not exist "%MSYS%\bin\g++.exe"    ( echo [error] MinGW not found at %MSYS% & exit /b 1 )
if not exist "%VSCMAKE%\cmake.exe"   ( echo [error] cmake not found at %VSCMAKE% & exit /b 1 )
if not exist "%MSYS%\bin\mingw32-make.exe" ( echo [error] mingw32-make not found & exit /b 1 )

set "PATH=%MSYS%\bin;%PATH%"
set "BUILD=%ROOT%\build-libretro"
set "MSYSU=%MSYS:\=/%"
set "ROOTU=%ROOT:\=/%"

if "%CLEAN%"=="1" if exist "%BUILD%" rmdir /s /q "%BUILD%"

set "EXTRA="
if "%OPENMPT%"=="ON" set "EXTRA=-DLIBRETRO_OPENMPT=ON -DLIBRETRO_OPENMPT_ROOT=%ROOTU%/third_party/libopenmpt"

echo == configure (%BUILD_TYPE%, openmpt=%OPENMPT%) ==
"%VSCMAKE%\cmake.exe" -G "MinGW Makefiles" -S "%ROOT%" -B "%BUILD%" ^
  -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
  -DCMAKE_C_COMPILER=%MSYSU%/bin/gcc.exe ^
  -DCMAKE_CXX_COMPILER=%MSYSU%/bin/g++.exe ^
  -DCMAKE_MAKE_PROGRAM=%MSYSU%/bin/mingw32-make.exe ^
  -DCMAKE_PREFIX_PATH=%MSYSU% ^
  -DBUILD_STATIC=ON ^
  -DLIBRETRO=ON %EXTRA%
if errorlevel 1 ( echo [error] configure failed & exit /b 1 )

echo == build ==
"%VSCMAKE%\cmake.exe" --build "%BUILD%" --parallel
if errorlevel 1 ( echo [error] build failed & exit /b 1 )

rem Stage the core the way a frontend wants it: <root>\dist\cores\pvz_libretro.dll
rem next to <root>\dist\system\pvz\ (the game data, which cannot be committed).
set "DIST=%ROOT%\dist\cores"
if not exist "%DIST%" mkdir "%DIST%"
copy /y "%BUILD%\pvz_libretro.dll" "%DIST%\pvz_libretro.dll" >nul
if not exist "%ROOT%\dist\info" mkdir "%ROOT%\dist\info"
if exist "%ROOT%\tools\libretro\pvz_libretro.info" copy /y "%ROOT%\tools\libretro\pvz_libretro.info" "%ROOT%\dist\info\pvz_libretro.info" >nul

echo == done: %BUILD%\pvz_libretro.dll ==
echo          staged: %DIST%\pvz_libretro.dll
endlocal
