# Builds bin\QuarryDLSS.addon64 with MSVC (Visual Studio 2022+ with C++ tools).
# MSVC is required: ReShade is built with it, and MinGW returns API structs
# (resource, resource_desc, ...) differently, which crashes inside ReShade.
# Usage: .\build.ps1 [-Install]   (-Install copies it next to TheQuarry-Win64-Shipping.exe)
param(
    [switch]$Install,
    [string]$GameBin = "E:\SteamLibrary\steamapps\common\The Quarry\SMG026\Binaries\Win64"
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with the C++ x64 tools not found." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"

$obj = "$root\bin\obj"
New-Item -ItemType Directory -Force $obj | Out-Null

# ReShade add-on headers in third_party/ match ReShade 6.8.0.
$cmd = @(
    "call `"$vcvars`" >nul",
    "rc /nologo /i `"$root\src`" /fo `"$obj\version.res`" `"$root\src\version.rc`"",
    ("cl /nologo /std:c++17 /O2 /EHsc /MT /W3 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX " +
     "/I `"$root\third_party\imgui`" /I `"$root\third_party\reshade\include`" " +
     "/Fo`"$obj\\`" `"$root\src\probe.cpp`" `"$obj\version.res`" " +
     "/link /DLL /OUT:`"$root\bin\QuarryDLSS.addon64`" /PDB:`"$obj\QuarryDLSS.pdb`" /DEBUG")
) -join " && "
cmd /c $cmd
if ($LASTEXITCODE) { throw "add-on build failed" }
Write-Host "Built $root\bin\QuarryDLSS.addon64"

if ($Install) {
    if (-not (Test-Path "$GameBin\TheQuarry-Win64-Shipping.exe")) { throw "Game not found in $GameBin" }
    if (Get-Process TheQuarry-Win64-Shipping -ErrorAction SilentlyContinue) { throw "Close the game first." }
    Copy-Item "$root\bin\QuarryDLSS.addon64" $GameBin -Force
    Write-Host "Installed to $GameBin"
}
