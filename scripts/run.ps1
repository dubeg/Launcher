param(
    [string]$Configuration = "Debug"
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Exe = Join-Path $Root "build\$Configuration\launcher.exe"

if (-not (Test-Path $Exe)) {
    & (Join-Path $Root "scripts\build.ps1") -Configuration $Configuration
}

Start-Process -FilePath $Exe -WorkingDirectory (Split-Path $Exe -Parent)
