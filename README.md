# Taskbar Tray Icon Resizer

[![Build](https://github.com/JulienMaille/small-task-icons/actions/workflows/build.yml/badge.svg)](https://github.com/JulienMaille/small-task-icons/actions/workflows/build.yml)

Resize Windows 11 taskbar tray icons from the default 32px down to **24px** (or any size you want).

A **standalone** adaptation of the excellent [Windhawk](https://www.windhawk.net/) mod *"Taskbar tray icon spacing and grid"* by [m417z](https://github.com/m417z). No Windhawk required.

![Tray icon width: 24](https://i.imgur.com/EIyWATk.png)

## Download

Grab the latest build from the [Releases](https://github.com/JulienMaille/small-task-icons/releases) page or the **Actions** tab.

## Usage

1. Place `tray_resizer.exe` and `tray_resizer_dll.dll` in the same folder
2. **Run `tray_resizer.exe` as Administrator**
3. Tray icons will be resized to 24px within a few seconds

To **stop**: restart `explorer.exe` in Task Manager, or log out/in.

## Customize icon size

Edit `source/tray_resizer_dll.cpp` — change the `Config` namespace:

```cpp
namespace Config {
    constexpr int kIconWidth      = 24;   // ← your preferred size
    constexpr int kIconRows       = 1;    // ← 2+ for a grid layout
    constexpr int kOverflowWidth  = 32;   // ← overflow popup icon size
    constexpr int kOverflowPerRow = 5;    // ← icons per row in overflow
}
```

Then rebuild (handled automatically by CI if you push).

## How it works

- **tray_resizer_dll.dll** gets injected into `explorer.exe`
- It walks the taskbar's XAML visual tree and shrinks tray icon elements (same technique as the original Windhawk mod)
- It periodically reapplies styles (~4s) to catch newly appearing icons

## Build from source

Requires **Visual Studio 2022+** with "Desktop development with C++" workload.

```
git clone https://github.com/JulienMaille/small-task-icons.git
cd small-task-icons\source
build.bat
```

Or just push to GitHub — the CI action builds everything automatically.

## Credits

- Original Windhawk mod: [m417z / Taskbar tray icon spacing and grid](https://github.com/m417z/my-windhawk-mods)
- License: **GNU GPL v3**
