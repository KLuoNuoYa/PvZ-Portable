@echo off
rem ---------------------------------------------------------------------------
rem Build libopenmpt as a static MinGW library for the PvZ libretro core.
rem
rem Why this exists: the game's sounds/mainmusic.mo3 stores its samples Ogg
rem Vorbis compressed, and libopenmpt is the only tracker backend that both
rem registers MO3 and implements the per-channel stem API the adaptive music
rem needs.  libmodplug cannot load MO3 at all (verified: ModPlug_Load returns
rem NULL), and libopenmpt ships only an autotools build plus MSVC binaries --
rem neither of which links into a MinGW core.  So we compile the sources.
rem
rem Input : third_party\libopenmpt-<version>+release.autotools.tar.gz
rem Output: third_party\libopenmpt\{include,lib}
rem
rem Usage: tools\libretro\build_libopenmpt.bat [--clean]
rem ---------------------------------------------------------------------------
setlocal EnableDelayedExpansion

rem Two levels up: the repository root (this script lives in <root>\tools\libretro).
pushd "%~dp0..\.."
set "ROOT=%CD%"
popd

call "%~dp0_find_toolchain.bat" || exit /b 1

set "SRC=%ROOT%\third_party\libopenmpt-src"
set "GEN=%ROOT%\third_party\libopenmpt-cmake"
set "OUT=%ROOT%\third_party\libopenmpt"
set "BUILD=%ROOT%\build-libopenmpt"
set "CLEAN=0"
if /I "%~1"=="--clean" set "CLEAN=1"

if not exist "%MSYS%\bin\g++.exe"  ( echo [error] MinGW not found at %MSYS% & exit /b 1 )
if not exist "%VSCMAKE%\cmake.exe" ( echo [error] cmake not found at %VSCMAKE% & exit /b 1 )

rem --- 1. source package -----------------------------------------------------
if not exist "%SRC%\Makefile.am" (
    echo == extracting libopenmpt source package ==
    set "TARBALL="
    for %%F in ("%ROOT%\third_party\libopenmpt-*autotools.tar.gz" "%ROOT%\libopenmpt-*autotools.tar.gz") do (
        if exist "%%~fF" set "TARBALL=%%~fF"
    )
    if "!TARBALL!"=="" (
        echo [error] libopenmpt autotools source package not found.
        echo         Put libopenmpt-^<version^>+release.autotools.tar.gz in the repo root
        echo         or in third_party\, or extract it to third_party\libopenmpt-src yourself.
        exit /b 1
    )
    echo    using "!TARBALL!"
    if exist "%ROOT%\third_party\_ompt_extract" rmdir /s /q "%ROOT%\third_party\_ompt_extract"
    mkdir "%ROOT%\third_party\_ompt_extract"
    tar -xzf "!TARBALL!" -C "%ROOT%\third_party\_ompt_extract"
    if errorlevel 1 ( echo [error] tar failed & exit /b 1 )
    for /d %%D in ("%ROOT%\third_party\_ompt_extract\*") do move "%%~fD" "%SRC%" >nul
    rmdir /s /q "%ROOT%\third_party\_ompt_extract"
)

rem --- 2. source list --------------------------------------------------------
echo == generating source list ==
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\tools\libretro\gen_libopenmpt_sources.ps1" -Root "%ROOT%"
if errorlevel 1 ( echo [error] source list generation failed & exit /b 1 )

rem --- 3. configure / build / install ----------------------------------------
if "%CLEAN%"=="1" if exist "%BUILD%" rmdir /s /q "%BUILD%"

set "PATH=%MSYS%\bin;%PATH%"
set "MSYSU=%MSYS:\=/%"
set "OUTU=%OUT:\=/%"

echo == configure ==
"%VSCMAKE%\cmake.exe" -G "MinGW Makefiles" -S "%GEN%" -B "%BUILD%" ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=%MSYSU%/bin/gcc.exe ^
  -DCMAKE_CXX_COMPILER=%MSYSU%/bin/g++.exe ^
  -DCMAKE_MAKE_PROGRAM=%MSYSU%/bin/mingw32-make.exe ^
  -DCMAKE_PREFIX_PATH=%MSYSU% ^
  -DCMAKE_INSTALL_PREFIX=%OUTU%
if errorlevel 1 ( echo [error] configure failed & exit /b 1 )

echo == build (about 150 translation units, be patient) ==
"%VSCMAKE%\cmake.exe" --build "%BUILD%" --parallel
if errorlevel 1 ( echo [error] build failed & exit /b 1 )

echo == install ==
"%VSCMAKE%\cmake.exe" --install "%BUILD%"
if errorlevel 1 ( echo [error] install failed & exit /b 1 )

echo == done: %OUT%\lib\libopenmpt.a ==
echo    now rebuild the core with:  tools\libretro\build_libretro.bat Release --openmpt
endlocal
