@echo off
rem ---------------------------------------------------------------------------
rem Locates the Windows build toolchain and exports these variables to the
rem caller (via `call`):
rem
rem   MSYS     MinGW-w64 root, e.g. <msys2>\mingw64      (gcc, g++, SDL2, zlib...)
rem   VSCMAKE  directory containing cmake.exe
rem   VSNINJA  directory containing ninja.exe           (optional)
rem
rem Nothing here is tied to a particular machine: every value can be overridden
rem from the environment, and detection probes canonical layout names on every
rem drive rather than any one developer's install path.
rem
rem   set MSYS=C:\path\to\mingw64
rem   set VSCMAKE=C:\path\to\cmake\bin
rem
rem Detection order:
rem   MSYS     %MSYS%  ->  g++ on PATH  ->  <drive>:\msys64\{mingw64,ucrt64,clang64}
rem   VSCMAKE  %VSCMAKE%  ->  vswhere  ->  <drive>:\Program Files\Microsoft Visual Studio\2022\*
rem ---------------------------------------------------------------------------

if defined MSYS if exist "%MSYS%\bin\g++.exe" goto :have_msys

rem g++ already on PATH?  Derive the MSYS root from it.
for /f "delims=" %%G in ('where g++ 2^>nul') do (
    if exist "%%~dpGmingw32-make.exe" (
        pushd "%%~dpG.."
        set "MSYS=%CD%"
        popd
        goto :have_msys
    )
)

for %%D in (C D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
    if exist "%%D:\msys64\mingw64\bin\g++.exe" (
        set "MSYS=%%D:\msys64\mingw64"
        goto :have_msys
    )
    if exist "%%D:\msys64\ucrt64\bin\g++.exe" (
        set "MSYS=%%D:\msys64\ucrt64"
        goto :have_msys
    )
    if exist "%%D:\msys64\clang64\bin\g++.exe" (
        set "MSYS=%%D:\msys64\clang64"
        goto :have_msys
    )
    if exist "%%D:\msys32\mingw32\bin\g++.exe" (
        set "MSYS=%%D:\msys32\mingw32"
        goto :have_msys
    )
)

echo [error] MinGW-w64 not found. Install MSYS2, or point MSYS at a MinGW root:
echo         set MSYS=C:\path\to\mingw64
exit /b 1

:have_msys
if not exist "%MSYS%\bin\mingw32-make.exe" (
    echo [error] mingw32-make not found under %MSYS%\bin  ^(pacman -S mingw-w64-x86_64-make^)
    exit /b 1
)

if defined VSCMAKE if exist "%VSCMAKE%\cmake.exe" goto :have_cmake

rem vswhere is the supported way to locate any Visual Studio installation.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -property installationPath 2^>nul`) do (
        if exist "%%I\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
            set "VSROOT=%%I"
            goto :have_vs
        )
    )
)

for %%D in (C D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
    for %%E in (Community Professional Enterprise BuildTools) do (
        if exist "%%D:\Program Files\Microsoft Visual Studio\2022\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
            set "VSROOT=%%D:\Program Files\Microsoft Visual Studio\2022\%%E"
            goto :have_vs
        )
        if exist "%%D:\Program Files (x86)\Microsoft Visual Studio\2022\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
            set "VSROOT=%%D:\Program Files (x86)\Microsoft Visual Studio\2022\%%E"
            goto :have_vs
        )
    )
)

echo [error] CMake not found. Install the "C++ CMake tools for Windows" workload,
echo         or point VSCMAKE at a directory containing cmake.exe:
echo         set VSCMAKE=C:\path\to\cmake\bin
exit /b 1

:have_vs
set "VSCMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
if exist "%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" (
    set "VSNINJA=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
)
exit /b 0
