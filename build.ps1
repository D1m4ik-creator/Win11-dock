$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $root 'src'
$out = Join-Path $root 'bin'
$gxx = 'C:\msys64\mingw64\bin\g++.exe'

if (-not (Test-Path $gxx)) { throw "g++ not found at $gxx" }
New-Item -ItemType Directory -Force -Path $out | Out-Null

# A running copy holds the exe open; the linker cannot overwrite it.
Get-Process win11-dock -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 250

$sources = Get-ChildItem -Path $src -Filter *.cpp | ForEach-Object { $_.FullName }

$defines = @(
    '-DWIN32_LEAN_AND_MEAN',
    '-DNOMINMAX',
    '-DUNICODE',
    '-D_UNICODE'
)

$cflags = @(
    '-std=c++20',
    '-O2',
    '-municode',
    '-Wall',
    '-Wextra',
    '-Wno-unknown-pragmas',
    '-Wno-unused-parameter',
    '-Wno-cast-function-type',
    '-fno-exceptions',
    '-fno-rtti'
)

$libs = @(
    '-ld2d1',
    '-ld3d11',
    '-ldxgi',
    '-ldcomp',
    '-ldwrite',
    '-lwindowscodecs',
    '-lole32',
    '-luuid',
    '-liphlpapi',
    '-lws2_32',
    '-lpowrprof',
    '-lpdh',
    '-lshlwapi',
    '-lshcore',
    '-lcomctl32',
    '-lgdi32',
    '-luser32',
    '-lshell32',
    '-ladvapi32',
    '-ldwmapi'
)

$exe = Join-Path $out 'win11-dock.exe'

Write-Host "Compiling $($sources.Count) files..." -ForegroundColor Cyan
& $gxx @cflags @defines '-Isrc' $sources '-o' $exe '-mwindows' @libs
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }

Write-Host "Built: $exe" -ForegroundColor Green
Get-Item $exe | Select-Object Name, Length, LastWriteTime
