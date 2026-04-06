param(
    [string]$Configuration = "Debug"
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$BuildDir = Join-Path $Root "build\$Configuration"
$CMakeExe = "C:\Program Files\CMake\bin\cmake.exe"
$VcVars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
$LlvmBin = "C:\Program Files\LLVM\bin"

if (-not (Test-Path (Join-Path $BuildDir "build.ninja"))) {
    & (Join-Path $Root "scripts\configure.ps1") -Configuration $Configuration
}

$cmd = "call `"$VcVars`" >nul && set PATH=$LlvmBin;%PATH% && `"$CMakeExe`" --build `"$BuildDir`""
cmd.exe /c $cmd
