# Clipper

Windows utility that masks keystrokes in password fields and terminals with random decoy input to deter shoulder surfing and casual observation.

While active, each real keypress in a targeted field triggers a burst of random decoy keystrokes followed by Backspace cleanup. The actual character you typed still lands correctly, but what appears on screen is much harder to read at a glance.

## How it works

Clipper runs in the background and uses:

- A low-level keyboard hook to observe key events
- UI Automation to detect focused password fields, text inputs, and console windows
- `SendInput` / `WriteConsoleInput` to inject decoy keys, then clean up with Backspace

**Default targets:** password fields and consoles (CMD, PowerShell, Windows Terminal, etc.)

**Optional `--all` mode:** all editable text inputs (not just passwords)

Clipper is active immediately on launch. Use the hotkeys below to pause, change mode, or exit.

## Requirements

- Windows 10 or later
- Administrator privileges for first run (installation and optional Windows Security credential UI support)
- A C++17 toolchain to build from source (see below)

## Building

The easiest path is `build-clip.bat`, which picks a compiler automatically and signs the output when possible.

```bat
build-clip.bat
```

Output: `clip.exe` in the project root.

### MinGW (g++)

Install [MSYS2](https://www.msys2.org/) or another MinGW distribution and ensure `g++` and `windres` are on your `PATH`.

```bat
build-clip.bat
```

The script will:

1. Create a local code-signing certificate if needed (`create-signing-cert.ps1`)
2. Compile resources with `windres`
3. Link with `g++` (C++17, Unicode, Windows subsystem)
4. Sign `clip.exe` (`sign-clip.ps1`)

Manual MinGW build:

```bat
windres clip.rc -O coff -o clip.res
g++ -std=c++17 -O2 -Wall -Wextra -municode -mwindows clip.cpp clip.res -o clip.exe ^
  -luser32 -lkernel32 -lshell32 -ladvapi32 -lole32 -loleaut32 -luiautomationcore -luuid -lcrypt32 ^
  -static
del clip.res
```

### LLVM clang++

If `C:\Program Files\LLVM\bin\clang++.exe` is present, `build-clip.bat` uses it as a fallback when MinGW is not found.

### CMake (Visual Studio / MSVC)

```bat
cmake -S . -B build
cmake --build build --config Release
copy build\Release\clip.exe clip.exe
```

Or let `build-clip.bat` invoke CMake when neither MinGW nor LLVM is available.

### Code signing

`build-clip.bat` runs `sign-clip.ps1`, which creates a self-signed **Hickory Phantom** certificate (first build only) and signs `clip.exe`.

- `*.pfx` is gitignored — generated locally, never commit it
- `hickory-phantom-signing.cer` is also generated locally and embedded in the binary for publisher trust

To install cert trust on a machine manually (optional; Clipper also does this on first elevated run):

```powershell
# Run as Administrator
.\install-signing-trust.ps1
```

## Installation and startup

**First run** (from any location, as Administrator):

1. Copies itself to `%LOCALAPPDATA%\Hickory Phantom\Clipper\clip.exe`
2. Registers startup so it launches after you sign in
3. Relaunches from the install folder and exits the original process

Startup is configured two ways (either one succeeding is enough):

| Mechanism | Details |
|-----------|---------|
| Registry Run key | `HKCU\...\Run` value `Clipper` |
| Scheduled task | `HickoryPhantomClipper`, trigger at logon with 15s delay |

If you pass `--all` on first launch, that flag is preserved in the startup entry.

Subsequent runs use the installed copy under Local AppData. Only one instance runs at a time (global mutex).

## Usage

### Running

```bat
clip.exe
clip.exe --all
```

Run as Administrator when you need Windows Security / credential prompt support.

### Hotkeys

All hotkeys use **Ctrl+Shift+Alt**:

| Key | Action |
|-----|--------|
| **S** | Start (resume masking) |
| **F** | End (pause masking) |
| **E** | Toggle `pwd+console` ↔ `all` inputs |
| **H** | Show / hide debug console |
| **C** | Close Clipper |

Press **Ctrl+Shift+Alt+H** to open the console and see status messages.

### Modes

| Mode | Scope |
|------|-------|
| `pwd+console` (default) | Password fields + consoles |
| `all` | All editable text inputs |

Toggle at runtime with **Ctrl+Shift+Alt+E**, or start with `clip.exe --all`.

### Behavior notes

- Injects **12–22** decoy keys per real keypress (two independent random counts: low-level fakes + visible window keys)
- Skips injection while **Shift** is held (left, right, or generic)
- Skips when Ctrl, Alt, or Win modifiers are down
- Console windows use `WriteConsoleInput` with a `SendInput` fallback
- Uses native UI Automation — no PowerShell dependency at runtime

### Environment variables

| Variable | Default | Description |
|----------|---------|-------------|
| `CLIP_MIN_KEYS` | `12` | Minimum decoy keys per press |
| `CLIP_MAX_KEYS` | `22` | Maximum decoy keys per press |
| `CLIP_IGNORE_MS` | `80` | Hotkey debounce (ms) |
| `CLIP_PASSWORD_POLL_MS` | `250` | Focus poll interval (ms) |
| `CLIP_WINDOW_KEY_MS` | `0` | Delay between window key injections |
| `CLIP_CONSOLE_KEY_MS` | `0` | Delay between console key injections |
| `CLIP_CONSOLE_DEFER_MS` | `20` | Console inject defer timer (ms) |
| `CLIP_DEBUG` | off | Set to `1` to log skip reasons |

Example:

```bat
set CLIP_DEBUG=1
set CLIP_MIN_KEYS=8
set CLIP_MAX_KEYS=16
clip.exe
```

## Project layout

| File | Purpose |
|------|---------|
| `clip.cpp` | Main application |
| `clip.rc` / `clip.manifest` | Version info, embedded cert, manifest |
| `CMakeLists.txt` | CMake build |
| `build-clip.bat` | One-shot build script |
| `create-signing-cert.ps1` | Generate self-signed signing cert |
| `sign-clip.ps1` | Sign `clip.exe` |
| `install-signing-trust.ps1` | Install cert into trusted stores |
| `diag-signing.ps1` | Signing diagnostics |

## Uninstalling

1. Remove startup entries:
   - Delete `Clipper` from `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`
   - Unregister scheduled task `HickoryPhantomClipper`
2. Delete `%LOCALAPPDATA%\Hickory Phantom\Clipper\`
3. End `clip.exe` via Task Manager or **Ctrl+Shift+Alt+C**

## Security note

Clipper is a **shoulder-surfing deterrent**, not encryption or anti-malware. It does not protect against dedicated keyloggers, memory inspection, or remote access tools with full system visibility. Use it as one layer of physical-privacy hygiene, not as a substitute for proper credential management.

## License

Copyright (C) Hickory Phantom
