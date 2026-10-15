# Builds bin\QuarryDLSS.addon64 with LLVM-MinGW (winget: MartinStorsjo.LLVM-MinGW.UCRT).
# Usage: .\build.ps1 [-Install]   (-Install copies it next to TheQuarry-Win64-Shipping.exe)
param(
    [switch]$Install,
    [string]$GameBin = "E:\SteamLibrary\steamapps\common\The Quarry\SMG026\Binaries\Win64"
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$cxx = (Get-Command clang++ -ErrorAction SilentlyContinue).Source
if (-not $cxx) {
    $cxx = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW*\*\bin\clang++.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $cxx) { throw "clang++ not found. Install with: winget install MartinStorsjo.LLVM-MinGW.UCRT" }

New-Item -ItemType Directory -Force "$root\bin\obj" | Out-Null
$windres = Join-Path (Split-Path $cxx) "llvm-windres.exe"
& $windres --target=pe-x86-64 -I "$root\src" -i "$root\src\version.rc" -O coff -o "$root\bin\obj\version.o"
if ($LASTEXITCODE) { throw "resource compile failed" }

# ReShade add-on headers in third_party/ match ReShade 6.8.0.
& $cxx --target=x86_64-w64-mingw32 -std=c++17 -O2 -shared -static -s `
    -Wall -Wno-unused-function -Wno-missing-braces -Wno-unknown-attributes `
    -I "$root\third_party\imgui" -I "$root\third_party\reshade\include" `
    -o "$root\bin\QuarryDLSS.addon64" "$root\src\probe.cpp" "$root\bin\obj\version.o"
if ($LASTEXITCODE) { throw "add-on build failed" }
Write-Host "Built $root\bin\QuarryDLSS.addon64"

if ($Install) {
    if (-not (Test-Path "$GameBin\TheQuarry-Win64-Shipping.exe")) { throw "Game not found in $GameBin" }
    Copy-Item "$root\bin\QuarryDLSS.addon64" $GameBin -Force
    Write-Host "Installed to $GameBin"
}
