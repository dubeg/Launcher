# Launcher

<img width="1150" height="825" alt="image" src="https://github.com/user-attachments/assets/1be2ae51-2ea4-4f1e-b4d8-f517345ca965" />


Fast Windows 11 launcher written in C with:

- Win32 windowing/input
- DirectX 11 rendering
- `kb_text_shape.h` shaping
- arena-based memory ownership
- Start Menu + `System32` app discovery
- Everything-backed file search

## Prerequisites

- Windows 11 SDK
- Visual Studio 2022 Build Tools or Community with MSVC and Windows SDK installed
- `cmake`
- `ninja`
- `clang-cl` from LLVM
- Everything running for live file-search results

## Quick Start

```powershell
pwsh -ExecutionPolicy Bypass -File .\scripts\configure.ps1
pwsh -ExecutionPolicy Bypass -File .\scripts\build.ps1
pwsh -ExecutionPolicy Bypass -File .\scripts\run.ps1
```

## Smoke Test

```powershell
pwsh -ExecutionPolicy Bypass -File .\scripts\smoke-test.ps1
```

The smoke test starts the launcher, waits for the window, sends `Alt+Space`, types into it with `SendInput`, and verifies that the process stays alive.

## Config

- Extra application directories: `config/locations.json` — each entry in `paths` is either a string (`"D:\\Apps"`) or an object `{ "path": "...", "recursive": false }`. Plain strings and `"recursive": false` only list `.exe`/`.msc`/`.cpl`/`.com`/`.bat`/`.cmd` in that folder; `"recursive": true` walks subfolders (skips junctions/symlink dirs), capped at 40k items per root.
- Friendly `System32` aliases: `data/system_aliases.json`

## Notes

- The launcher uses `Segoe UI` from `%WINDIR%\Fonts\segoeui.ttf` by default.
- File mode degrades gracefully when Everything IPC is unavailable.
