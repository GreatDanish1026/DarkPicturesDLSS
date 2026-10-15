# Builds bin\QuarryDLSS.addon64 with MSVC (Visual Studio 2022+ with C++ tools).
# MSVC is required: ReShade is built with it, and MinGW returns API structs
# (resource, resource_desc, ...) differently, which crashes inside ReShade.
# Usage: .\build.ps1 [-Test] [-Install]
#   -Test     also builds and runs tests\dxbc_tests.cpp
#   -Install  copies the add-on next to TheQuarry-Win64-Shipping.exe, plus nvngx_dlss.dll
#             from -DlssDll if the game folder doesn't have that version yet
param(
    [switch]$Install,
    [switch]$Test,
    [string]$GameBin = "E:\SteamLibrary\steamapps\common\The Quarry\SMG026\Binaries\Win64",
    # DLSS runtime matching the SDK in third_party\dlss (310.9.1). It isn't redistributed in this repo.
    [string]$DlssDll = "E:\Non-Steam Games\The Witcher 3 Wild Hunt\bin\x64_dx12\nvngx_dlss.dll"
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with the C++ x64 tools not found." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"

$obj = "$root\bin\obj"
New-Item -ItemType Directory -Force $obj | Out-Null

# Prepare pass shader -> header (cs_5_0; D3D12 accepts SM 5.0 DXBC).
$fxc = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\fxc.exe" -ErrorAction SilentlyContinue |
    Sort-Object FullName | Select-Object -Last 1 -ExpandProperty FullName
if (-not $fxc) { throw "fxc.exe not found (Windows 10/11 SDK)." }
& $fxc /nologo /T cs_5_0 /E main /O3 /Vn kDlssPrepareCS /Fh "$obj\dlss_prepare_cs.h" "$root\src\shaders\dlss_prepare.hlsl" | Out-Null
if ($LASTEXITCODE) { throw "shader compile failed: dlss_prepare.hlsl" }
& $fxc /nologo /T cs_5_0 /E main /O3 /Vn kDlssClampCS /Fh "$obj\dlss_clamp_cs.h" "$root\src\shaders\dlss_clamp.hlsl" | Out-Null
if ($LASTEXITCODE) { throw "shader compile failed: dlss_clamp.hlsl" }

# ReShade add-on headers in third_party\reshade match ReShade 6.8.0; the DLSS SDK in third_party\dlss is 310.9.1.
$cmd = @(
    "call `"$vcvars`" >nul",
    "rc /nologo /i `"$root\src`" /fo `"$obj\version.res`" `"$root\src\version.rc`"",
    ("cl /nologo /std:c++17 /O2 /EHsc /MT /W3 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX " +
     "/I `"$root\third_party\imgui`" /I `"$root\third_party\reshade\include`" " +
     "/I `"$root\third_party\dlss\include`" /I `"$obj`" " +
     "/Fo`"$obj\\`" `"$root\src\probe.cpp`" `"$root\src\dlss.cpp`" `"$obj\version.res`" " +
     "/link /DLL /OUT:`"$root\bin\QuarryDLSS.addon64`" /PDB:`"$obj\QuarryDLSS.pdb`" /DEBUG " +
     "/LIBPATH:`"$root\third_party\dlss\lib`"")
) -join " && "
cmd /c $cmd
if ($LASTEXITCODE) { throw "add-on build failed" }
Write-Host "Built $root\bin\QuarryDLSS.addon64"

if ($Test) {
    $testCmd = @(
        "call `"$vcvars`" >nul",
        ("cl /nologo /std:c++17 /O2 /EHsc /MT /W3 /Fo`"$obj\\`" `"$root\tests\dxbc_tests.cpp`" " +
         "/link /OUT:`"$root\bin\dxbc_tests.exe`""),
        "`"$root\bin\dxbc_tests.exe`""
    ) -join " && "
    cmd /c $testCmd
    if ($LASTEXITCODE) { throw "tests failed" }
}

if ($Install) {
    if (-not (Test-Path "$GameBin\TheQuarry-Win64-Shipping.exe")) { throw "Game not found in $GameBin" }
    if (Get-Process TheQuarry-Win64-Shipping -ErrorAction SilentlyContinue) { throw "Close the game first." }
    Copy-Item "$root\bin\QuarryDLSS.addon64" $GameBin -Force
    $have = Get-Item "$GameBin\nvngx_dlss.dll" -ErrorAction SilentlyContinue
    if (-not $have -or $have.VersionInfo.FileVersion -ne (Get-Item $DlssDll).VersionInfo.FileVersion) {
        if (-not (Test-Path $DlssDll)) { throw "nvngx_dlss.dll not found at $DlssDll (pass -DlssDll)" }
        Copy-Item $DlssDll $GameBin -Force
        Write-Host "Copied nvngx_dlss.dll $((Get-Item $DlssDll).VersionInfo.FileVersion) from $DlssDll"
    }
    Write-Host "Installed to $GameBin"
}
