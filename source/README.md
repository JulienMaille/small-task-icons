# Taskbar Tray Icon Resizer

A **standalone** adaptation of the [Windhawk](https://www.windhawk.net/) mod *"Taskbar tray icon spacing and grid"* by m417z.  
No Windhawk required — just a simple EXE you run once.

## What it does

Resizes the Windows 11 taskbar notification area (system tray) icons from the default 32px to **24px** (configurable).

## How it works

1. **tray_resizer.exe** — A small C# injector that:
   - Finds `explorer.exe`
   - Injects `tray_resizer_dll.dll` into it via `CreateRemoteThread` + `LoadLibraryW`
   
2. **tray_resizer_dll.dll** — The worker DLL that:
   - Walks the taskbar's XAML visual tree
   - Sets `MinWidth` and other layout properties on tray icon elements
   - Periodically re-applies styles (every ~4s) to catch new icons

## Requirements

- **Windows 11 22H2 or newer** (same as the original mod)
- **x64 (64-bit)** system
- **Visual Studio 2022+** with the **"Desktop development with C++"** workload
  (to compile the DLL — or use a [pre-built release](#pre-built-binaries))

## Quick Start

### Option 1: Build from source

Open **"x64 Native Tools Command Prompt for VS 2022"** and run:

```batch
cd d:\Codez\small-task-icons\source
build.bat
```

Then run **as Administrator**:

```batch
tray_resizer.exe
```

### Option 2: Use a pre-built release

1. Download `tray_resizer_dll.dll` and `tray_resizer.exe` from the [Releases page](#)
2. Place them in the same folder
3. Run `tray_resizer.exe` **as Administrator**

## Customizing the icon size

Edit `tray_resizer_dll.cpp` and change the values in the `Config` namespace:

```cpp
namespace Config {
    constexpr int kIconWidth      = 24;   // ← Change this
    constexpr int kIconRows       = 1;    // ← Grid rows (1 = single row)
    constexpr int kOverflowWidth  = 32;   // ← Overflow popup icon size
    constexpr int kOverflowPerRow = 5;    // ← Icons per row in overflow
    constexpr int kReapplyMs      = 4000; // ← Recheck interval
}
```

Then rebuild with `build.bat`.

## Stopping the resizer

- **Restart explorer.exe** in Task Manager
- Or log out and back in
- The DLL will stay loaded until explorer.exe restarts

## How it compares to the original Windhawk mod

| Feature | Windhawk mod | This standalone app |
|---------|-------------|-------------------|
| Requires Windhawk | ✅ Yes | ❌ No |
| Icon size | ✅ Live configurable | ⚙️ Compile-time configurable |
| Grid (multi-row) | ✅ Yes | ✅ Yes (configurable) |
| Overflow popup | ✅ Yes | ✅ Yes |
| Catches new icons | ✅ Via function hooks | ✅ Via periodic reapplication |
| Build required | ❌ No (mod manager) | ✅ Yes (VS 2022 needed) |

## Technical notes

- The DLL uses the same technique as the original mod to access the taskbar's
  XAML root: scanning `taskbar.dll` for internal function signatures at runtime.
- Pattern scanning is done for `CTaskBand::GetTaskbarHost`, 
  `TaskbarHost::FrameHeight`, and `std::_Ref_count_base::_Decref`.
- Since constructor hooking requires function-level detours (complex without
  a library like minhook/detours), this version uses a timer-based approach
  that re-applies styles every ~4 seconds instead.

## Files

| File | Description |
|------|-------------|
| `source/tray_resizer_dll.cpp` | Worker DLL source (C++/WinRT) |
| `source/tray_resizer.cs` | Injector source (C#) |
| `source/build.bat` | Build script |
| `source/README.md` | This file |

## Credits

- Original mod: [m417z](https://github.com/m417z/my-windhawk-mods) / [Windhawk](https://www.windhawk.net/)
- Adapted to standalone by removing Windhawk dependency
- License: GPL v3 (as the original)
