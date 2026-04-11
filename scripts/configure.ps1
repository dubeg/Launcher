param(
    [string]$Configuration = "Debug"
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$BuildDir = Join-Path $Root "build\$Configuration"
$CMakeExe = "C:\Program Files\CMake\bin\cmake.exe"

$VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$VcVars = $null
if (Test-Path $VsWhere) {
    $installPath = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if ($installPath) {
        $candidate = Join-Path $installPath "VC\Auxiliary\Build\vcvars64.bat"
        if (Test-Path $candidate) { $VcVars = $candidate }
    }
}
if (-not $VcVars) {
    $VcVars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
}

$NinjaExe = $null
foreach ($p in @(
        (Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe\ninja.exe"),
        (Join-Path $env:ProgramFiles "Ninja\ninja.exe")
    )) {
    if ($p -and (Test-Path $p)) {
        $NinjaExe = $p
        break
    }
}
if (-not $NinjaExe) {
    $cmdNinja = Get-Command ninja -ErrorAction SilentlyContinue
    if ($cmdNinja) { $NinjaExe = $cmdNinja.Source }
}

if (-not (Test-Path $VcVars)) {
    throw "vcvars64.bat not found. Install the MSVC C++ workload (VS 2022 or Build Tools), or fix VS path. Tried: $VcVars"
}

if (-not (Test-Path $CMakeExe)) {
    Write-Host "cmake not found. Install with: winget install --id Kitware.CMake -e"
    exit 1
}

if (-not $NinjaExe) {
    Write-Host "ninja not found. Install with: winget install --id Ninja-build.Ninja -e"
    exit 1
}

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

# MSVC (cl) via vcvars — avoids clang-cl + Windows SDK header quirks (e.g. SHGetPropertyStoreFromParsingName).
$Command = "call `"$VcVars`" >nul && `"$CMakeExe`" -S `"$Root`" -B `"$BuildDir`" -G Ninja -DCMAKE_BUILD_TYPE=$Configuration -DCMAKE_MAKE_PROGRAM=`"$NinjaExe`" -DCMAKE_C_COMPILER=cl"
cmd.exe /c $Command
