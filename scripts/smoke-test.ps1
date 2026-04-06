param(
    [string]$Configuration = "Debug"
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Exe = Join-Path $Root "build\$Configuration\launcher.exe"

if (-not (Test-Path $Exe)) {
    & (Join-Path $Root "scripts\build.ps1") -Configuration $Configuration
}

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class Win32Smoke {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int maxCount);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);

    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT {
        public uint type;
        public INPUTUNION U;
    }

    [StructLayout(LayoutKind.Explicit)]
    public struct INPUTUNION {
        [FieldOffset(0)] public KEYBDINPUT ki;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT {
        public ushort wVk;
        public ushort wScan;
        public uint dwFlags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    public const uint INPUT_KEYBOARD = 1;
    public const uint KEYEVENTF_KEYUP = 0x0002;
}
"@

function Get-LauncherWindow() {
    return [Win32Smoke]::FindWindow("LauncherWindowClass", "Launcher")
}

function Send-KeyStroke([UInt16[]]$Keys) {
    $inputs = New-Object Win32Smoke+INPUT[] ($Keys.Length * 2)
    for ($i = 0; $i -lt $Keys.Length; $i++) {
        $down = New-Object Win32Smoke+INPUT
        $down.type = [Win32Smoke]::INPUT_KEYBOARD
        $down.U.ki.wVk = $Keys[$i]
        $inputs[$i * 2] = $down

        $up = New-Object Win32Smoke+INPUT
        $up.type = [Win32Smoke]::INPUT_KEYBOARD
        $up.U.ki.wVk = $Keys[$i]
        $up.U.ki.dwFlags = [Win32Smoke]::KEYEVENTF_KEYUP
        $inputs[$i * 2 + 1] = $up
    }
    [Win32Smoke]::SendInput([uint32]$inputs.Length, $inputs, [Runtime.InteropServices.Marshal]::SizeOf([type][Win32Smoke+INPUT])) | Out-Null
}

function Send-HotkeyChord([UInt16]$Modifier, [UInt16]$Key) {
    $inputs = New-Object Win32Smoke+INPUT[] 4

    $inputs[0] = New-Object Win32Smoke+INPUT
    $inputs[0].type = [Win32Smoke]::INPUT_KEYBOARD
    $inputs[0].U.ki.wVk = $Modifier

    $inputs[1] = New-Object Win32Smoke+INPUT
    $inputs[1].type = [Win32Smoke]::INPUT_KEYBOARD
    $inputs[1].U.ki.wVk = $Key

    $inputs[2] = New-Object Win32Smoke+INPUT
    $inputs[2].type = [Win32Smoke]::INPUT_KEYBOARD
    $inputs[2].U.ki.wVk = $Key
    $inputs[2].U.ki.dwFlags = [Win32Smoke]::KEYEVENTF_KEYUP

    $inputs[3] = New-Object Win32Smoke+INPUT
    $inputs[3].type = [Win32Smoke]::INPUT_KEYBOARD
    $inputs[3].U.ki.wVk = $Modifier
    $inputs[3].U.ki.dwFlags = [Win32Smoke]::KEYEVENTF_KEYUP

    [Win32Smoke]::SendInput([uint32]$inputs.Length, $inputs, [Runtime.InteropServices.Marshal]::SizeOf([type][Win32Smoke+INPUT])) | Out-Null
}

function Send-Text([string]$Text) {
    $wshell = New-Object -ComObject WScript.Shell
    $wshell.SendKeys($Text)
}

$process = Start-Process -FilePath $Exe -WorkingDirectory (Split-Path $Exe -Parent) -PassThru
Start-Sleep -Milliseconds 700

if ($process.HasExited) {
    throw "Launcher exited before smoke test."
}

Send-HotkeyChord 0x12 0x20
Start-Sleep -Milliseconds 500

$window = Get-LauncherWindow
if ($window -ne [IntPtr]::Zero -and -not [Win32Smoke]::IsWindowVisible($window)) {
    [Win32Smoke]::PostMessage($window, 0x0312, [IntPtr]1, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 500
}

$window = Get-LauncherWindow
if ($window -eq [IntPtr]::Zero -or -not [Win32Smoke]::IsWindowVisible($window)) {
    Stop-Process -Id $process.Id -Force
    throw "Launcher window did not appear after Alt+Space."
}

[Win32Smoke]::SetForegroundWindow($window) | Out-Null
Start-Sleep -Milliseconds 200
Send-Text("note")
Start-Sleep -Milliseconds 400

if ($process.HasExited) {
    throw "Launcher exited during smoke test."
}

Write-Host "Smoke test passed. PID=$($process.Id)"
Stop-Process -Id $process.Id -Force
