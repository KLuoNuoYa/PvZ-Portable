# Regenerates third_party/libopenmpt-cmake/sources.cmake from the libopenmpt
# source package's Makefile.am.
#
# libopenmpt ships an autotools build (needs a POSIX shell) plus prebuilt MSVC
# DLLs, neither of which fits a MinGW-built libretro core, so the source list is
# lifted out of Makefile.am and compiled directly.  Run this after extracting a
# new libopenmpt release.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\libretro\gen_libopenmpt_sources.ps1

[CmdletBinding()]
param(
    # Two levels up: the repository root (this script lives in <root>\tools\libretro).
    [string]$Root = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
)

$ErrorActionPreference = 'Stop'

$src = Join-Path $Root 'third_party\libopenmpt-src'
$out = Join-Path $Root 'third_party\libopenmpt-cmake\sources.cmake'

if (-not (Test-Path (Join-Path $src 'Makefile.am'))) {
    throw "libopenmpt sources not found at $src (expected Makefile.am there)."
}

# Every 'MPT_FILES_xxx += path/to/file.cpp' line, plus the C API implementation
# files that Makefile.am lists separately.
$listed = Select-String -Path (Join-Path $src 'Makefile.am') `
        -Pattern '^\s*MPT_FILES_\w+\s*\+=\s*(\S+\.cpp)\s*$' |
    ForEach-Object { $_.Matches[0].Groups[1].Value }

$api = @(
    'libopenmpt/libopenmpt_c.cpp',
    'libopenmpt/libopenmpt_cxx.cpp',
    'libopenmpt/libopenmpt_ext_impl.cpp',
    'libopenmpt/libopenmpt_impl.cpp'
)

$all = ($listed + $api) | Sort-Object -Unique

$missing = @()
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# Generated from libopenmpt-src/Makefile.am by tools/gen_libopenmpt_sources.ps1 -- do not edit.')
$lines.Add('set(OMPT_SOURCES')
foreach ($f in $all) {
    if (Test-Path (Join-Path $src ($f -replace '/', '\'))) {
        $lines.Add("  `${OMPT_SRC}/$f")
    } else {
        $missing += $f
    }
}
$lines.Add(')')

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $out) | Out-Null
$lines | Set-Content -Encoding ascii $out

Write-Host "wrote $out ($($all.Count - $missing.Count) sources)"
if ($missing.Count) {
    Write-Warning "listed in Makefile.am but not present: $($missing -join ', ')"
}
