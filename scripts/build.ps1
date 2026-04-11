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

if (-not (Test-Path (Join-Path $BuildDir "build.ninja"))) {
    & (Join-Path $Root "scripts\configure.ps1") -Configuration $Configuration
}

if (-not (Test-Path $VcVars)) {
    throw "vcvars64.bat not found at $VcVars"
}

# LNK1168: linker cannot overwrite launcher.exe while it is running.
$launcherOut = [System.IO.Path]::GetFullPath((Join-Path $BuildDir "launcher.exe"))
Get-Process -Name "launcher" -ErrorAction SilentlyContinue | Where-Object { $_.Path -and ($_.Path -ieq $launcherOut) } | ForEach-Object {
    Write-Host "Stopping $($_.Path) (PID $($_.Id)) so the build can replace it."
    Stop-Process -Id $_.Id -Force
}

$cmd = "call `"$VcVars`" >nul && `"$CMakeExe`" --build `"$BuildDir`""
cmd.exe /c $cmd
