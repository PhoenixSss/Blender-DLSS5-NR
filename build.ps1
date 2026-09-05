# SPDX-License-Identifier: MIT
# build.ps1 — cl.exe-based build for the blender_dlss5 A1 probe.
#
# No CMake / Visual Studio project needed: discovers the newest MSVC
# toolset (incl. non-standard install drives) and the newest Windows SDK,
# then compiles three targets:
#   [1/3] bin\dlss5nr_shim.dll     (/Od MANDATORY — return-address validation)
#   [2/3] bin\dlss5nr_probe.exe
#   [3/3] bin\dlss5nr_unit_tests.exe
#
# Usage:  powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 [-Clean] [-SkipTests] [-Verbose]
#
# Policy (§3/§30): this script NEVER downloads or copies NVIDIA DLLs.
# The neural runtime (nvngx_dlssnr.dll) is user-provided at run time.

param(
    [switch]$Clean,
    [switch]$SkipTests,
    [switch]$Verbose
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$OutDir = Join-Path $ProjectRoot "bin"
$ObjDir = Join-Path $OutDir "obj"

function VerboseLog {
    param([string]$Msg)
    if ($Verbose) { Write-Host $Msg }
}

# ---- 1. Discover cl.exe ---------------------------------------------------

function Find-NewestCl {
    $roots = @()
    foreach ($r in @($env:ProgramFiles, ${env:ProgramFiles(x86)}, "D:\Program Files", "D:\Program Files (x86)", "C:\Program Files", "C:\Program Files (x86)")) {
        if ($r -and (Test-Path $r) -and ($roots -notcontains $r)) { $roots += $r }
    }
    $candidates = @()
    foreach ($root in $roots) {
        $base = Join-Path $root "Microsoft Visual Studio"
        if (-not (Test-Path $base)) { continue }
        Get-ChildItem -Path $base -Recurse -Filter cl.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match "VC\\Tools\\MSVC\\[^\\]+\\bin\\Hostx64\\x64\\cl\.exe$" } |
            ForEach-Object {
                $versionPart = $_.FullName -replace "^.*MSVC\\([^\\]+)\\bin\\Hostx64\\x64\\cl\.exe$", '$1'
                $candidates += [PSCustomObject]@{ Path = $_.FullName; Toolset = $versionPart }
            }
    }
    $newest = $candidates | Sort-Object { [version]($_.Toolset) } -Descending | Select-Object -First 1
    return $newest
}

$Cl = Find-NewestCl
if (-not $Cl) {
    Write-Error "No MSVC cl.exe found (searched Program Files and D:\Program Files). Install Visual Studio 2022+ with the C++ workload."
}
VerboseLog "cl.exe: $($Cl.Path)"

$MsvcRoot = $Cl.Path -replace "\\bin\\Hostx64\\x64\\cl\.exe$", ""
$MsvcBase = $MsvcRoot -replace "\\bin\\Hostx64\\x64$", ""

# ---- 2. Discover Windows SDK ----------------------------------------------

function Find-NewestSdk {
    $kitRoots = @(
        "C:\Program Files (x86)\Windows Kits\10",
        "D:\Program Files (x86)\Windows Kits\10",
        "D:\Windows Kits\10",
        "C:\Windows Kits\10"
    )
    $candidates = @()
    foreach ($kit in $kitRoots) {
        $incRoot = Join-Path $kit "Include"
        $libRoot = Join-Path $kit "Lib"
        if (-not (Test-Path $incRoot) -or -not (Test-Path $libRoot)) { continue }
        Get-ChildItem -Path $incRoot -Directory |
            ForEach-Object {
                $ver = $_.Name
                $incUm = Join-Path $_.FullName "um"
                $libUm = Join-Path $libRoot "$ver\um\x64"
                if ((Test-Path (Join-Path $incUm "d3d12.h")) -and (Test-Path (Join-Path $libUm "d3d12.lib"))) {
                    $candidates += [PSCustomObject]@{
                        Version = $ver
                        IncludeRoot = Join-Path $kit "Include\$ver"
                        LibRoot = Join-Path $kit "Lib\$ver"
                    }
                }
            }
    }
    return $candidates | Sort-Object { [version]($_.Version) } -Descending | Select-Object -First 1
}

$Sdk = Find-NewestSdk
if (-not $Sdk) {
    Write-Error "No Windows SDK with d3d12.h/d3d12.lib found."
}
VerboseLog "Windows SDK: $($Sdk.Version) at $($Sdk.IncludeRoot)"

# ---- 3. Toolchain environment ---------------------------------------------

$env:PATH = (Split-Path $Cl.Path) + ";" + $env:PATH

$incParts = @(
    (Join-Path $MsvcBase "include"),
    (Join-Path $Sdk.IncludeRoot "ucrt"),
    (Join-Path $Sdk.IncludeRoot "shared"),
    (Join-Path $Sdk.IncludeRoot "um"),
    (Join-Path $Sdk.IncludeRoot "winrt"),
    (Join-Path $Sdk.IncludeRoot "cppwinrt")
)
$incParts = $incParts | Where-Object { $_ -and (Test-Path $_) }
$env:INCLUDE = ($incParts -join ";")

$libParts = @(
    (Join-Path $MsvcBase "lib\x64"),
    (Join-Path $Sdk.LibRoot "ucrt\x64"),
    (Join-Path $Sdk.LibRoot "um\x64")
)
$libParts = $libParts | Where-Object { $_ -and (Test-Path $_) }
$env:LIB = ($libParts -join ";")

$CommonFlags = @("/nologo", "/utf-8", "/std:c++17", "/EHsc", "/MT", "/W4", "/DUNICODE", "/D_UNICODE", "/D_WIN32_WINNT=0x0A00", "/I`"$ProjectRoot`"")
$ShimFlags = @("/LD", "/Od")      # /Od is load-bearing (caller_shim.cpp)
$OptFlags = @("/O2")

# ---- 4. Compile ------------------------------------------------------------

New-Item -ItemType Directory -Force -Path $OutDir, $ObjDir | Out-Null

function Invoke-Cl {
    param(
        [string[]]$Sources,
        [string]$Output,
        [string[]]$ExtraFlags = @(),
        [string[]]$Libs = @(),
        [string[]]$LinkFlags = @()
    )
    # Compile each source separately (cl accepts a single /Fo per run),
    # then link the objects in one step.
    $objs = @()
    foreach ($s in $Sources) {
        $objName = [System.IO.Path]::GetFileNameWithoutExtension($s) + ".obj"
        $objPath = Join-Path $ObjDir $objName
        $objs += $objPath
        $clArgs = @($CommonFlags + $ExtraFlags + @("/Fo`"$objPath`"", "/c", "`"$s`""))
        VerboseLog ("cl " + ($clArgs -join " "))
        & $Cl.Path $clArgs
        if ($LASTEXITCODE -ne 0) {
            Write-Error "cl.exe failed on $s with exit code $LASTEXITCODE"
        }
    }
    $linkArgs = @($objs)
    if ($Libs.Count -gt 0) {
        $linkArgs += @("/link", "/OUT:`"$Output`"", "/SUBSYSTEM:CONSOLE") + $LinkFlags + $Libs
    } else {
        $linkArgs += @("/link", "/OUT:`"$Output`"") + $LinkFlags
    }
    VerboseLog ("cl (link) " + ($linkArgs -join " "))
    & $Cl.Path $linkArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Error "link failed with exit code $LASTEXITCODE"
    }
}

# NOTE: the shim module name must start with "nvngx" — the NR snippet
# runtime's caller validation rejects callers whose module name does not
# match the NGX loader pattern (verified: 0xBAD00002 otherwise).
Write-Host "[1/3] nvngx.dll_dlss5.dll (/Od — caller shim, optimization is FORBIDDEN here)"
Invoke-Cl -Sources @((Join-Path $ProjectRoot "backends\feature18\caller_shim.cpp")) `
          -Output (Join-Path $OutDir "nvngx.dll_dlss5.dll") `
          -ExtraFlags $ShimFlags `
          -LinkFlags @("/DLL")

$ProbeSources = @(
    (Join-Path $ProjectRoot "diagnostics\json_writer.cpp"),
    (Join-Path $ProjectRoot "diagnostics\gpu_info.cpp"),
    (Join-Path $ProjectRoot "diagnostics\file_identity.cpp"),
    (Join-Path $ProjectRoot "canonical\color_transform.cpp"),
    (Join-Path $ProjectRoot "backends\interface\backend_registry.cpp"),
    (Join-Path $ProjectRoot "backends\feature18\feature18_backend.cpp"),
    (Join-Path $ProjectRoot "probe\test_pattern.cpp"),
    (Join-Path $ProjectRoot "probe\png_io.cpp"),
    (Join-Path $ProjectRoot "probe\report.cpp"),
    (Join-Path $ProjectRoot "probe\main.cpp")
)
$ProbeLibs = @("d3d12.lib", "dxgi.lib", "bcrypt.lib", "wintrust.lib", "crypt32.lib", "version.lib", "ole32.lib")

Write-Host "[2/3] dlss5nr_probe.exe"
Invoke-Cl -Sources $ProbeSources `
          -Output (Join-Path $OutDir "dlss5nr_probe.exe") `
          -ExtraFlags $OptFlags `
          -Libs $ProbeLibs

if (-not $SkipTests) {
    $TestSources = @(
        (Join-Path $ProjectRoot "diagnostics\json_writer.cpp"),
        (Join-Path $ProjectRoot "diagnostics\gpu_info.cpp"),
        (Join-Path $ProjectRoot "canonical\color_transform.cpp"),
        (Join-Path $ProjectRoot "probe\test_pattern.cpp"),
        (Join-Path $ProjectRoot "tests\unit_tests.cpp")
    )
    Write-Host "[3/3] dlss5nr_unit_tests.exe"
    Invoke-Cl -Sources $TestSources `
              -Output (Join-Path $OutDir "dlss5nr_unit_tests.exe") `
              -ExtraFlags $OptFlags `
              -Libs @("dxgi.lib", "version.lib")
}

$BridgeSources = @(
    (Join-Path $ProjectRoot "native\nr_bridge.cpp"),
    (Join-Path $ProjectRoot "canonical\color_transform.cpp"),
    (Join-Path $ProjectRoot "backends\interface\backend_registry.cpp"),
    (Join-Path $ProjectRoot "backends\feature18\feature18_backend.cpp"),
    (Join-Path $ProjectRoot "diagnostics\gpu_info.cpp"),
    (Join-Path $ProjectRoot "diagnostics\file_identity.cpp")
)
Write-Host "[4/4] nr_bridge.dll (Blender ctypes bridge, A2)"
Invoke-Cl -Sources $BridgeSources `
          -Output (Join-Path $OutDir "nr_bridge.dll") `
          -ExtraFlags $OptFlags `
          -Libs @("d3d12.lib", "dxgi.lib", "bcrypt.lib", "wintrust.lib", "crypt32.lib", "version.lib", "ole32.lib") `
          -LinkFlags @("/DLL")

# ---- 5. Cleanup ------------------------------------------------------------

Get-ChildItem -Path $ProjectRoot -Recurse -Include *.obj, *.exp, *.lib -File |
    Where-Object { $_.FullName -notlike "$OutDir\*" } |
    Remove-Item -Force -ErrorAction SilentlyContinue

if ($Clean) {
    Remove-Item -Recurse -Force $ObjDir -ErrorAction SilentlyContinue
}

Write-Host "Build complete. Outputs in $OutDir"
