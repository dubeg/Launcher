param(
    [string]$Configuration = "Debug"
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$BuildDir = Join-Path $Root "build\$Configuration"
$VcVars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
$CMakeExe = "C:\Program Files\CMake\bin\cmake.exe"
$NinjaExe = "C:\Users\gdube\AppData\Local\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe\ninja.exe"
$LlvmBin = "C:\Program Files\LLVM\bin"

if (-not (Test-Path $VcVars)) {
    throw "vcvars64.bat not found at $VcVars"
}

if (-not (Test-Path $CMakeExe)) {
    Write-Host "cmake not found. Install with: winget install --id Kitware.CMake -e"
    exit 1
}

if (-not (Test-Path $NinjaExe)) {
    Write-Host "ninja not found. Install with: winget install --id Ninja-build.Ninja -e"
    exit 1
}

if (-not (Test-Path (Join-Path $LlvmBin "clang-cl.exe"))) {
    Write-Host "clang-cl not found. Install with: winget install --id LLVM.LLVM -e"
    exit 1
}

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

$Command = "call `"$VcVars`" >nul && set PATH=$LlvmBin;%PATH% && `"$CMakeExe`" -S `"$Root`" -B `"$BuildDir`" -G Ninja -DCMAKE_BUILD_TYPE=$Configuration -DCMAKE_MAKE_PROGRAM=`"$NinjaExe`" -DCMAKE_C_COMPILER=clang-cl"
$cmd = $Command
cmd.exe /c $cmd
