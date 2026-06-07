# Ferns Unlocker

A lightweight FPS unlocker and performance tweaker for the Ferns client, now with
an optional built-in **SSR + SSAO post-process shader** for a glossier, more
modern look.

## Features

- **Remove FPS cap** — patches the frame-time limiter in `Ferns.exe` to a target
  of your choice (default 9999).
- **Performance tweaks**
  - Boost process priority to *High*
  - Disable Windows power throttling (EcoQoS)
  - Pin to performance cores (Intel 12th gen+ hybrid CPUs)
- **Auto-reapply on rejoin** — re-runs the patch automatically when the game
  process restarts.
- **Shader (experimental)** — a one-click install of a screen-space
  post-process that adds:
  - **SSR** — screen-space reflections (water, glossy surfaces)
  - **SSAO** — ambient occlusion (contact shadows in crevices/corners)
  - a subtle bloom + colour-grade "shiny" pass

## Usage

1. Download `FernsUnlocker.exe` from the [Releases](../../releases) page.
2. **Run it as administrator** (required to read/write the game's memory).
3. Launch the Ferns client and join a game.
4. Press **apply** to remove the FPS cap and apply the performance tweaks.
   - Tick *auto-reapply on rejoin* to keep it applied across rejoins.
5. *(optional)* Press **install shader** to enable the SSR + SSAO post-process,
   then **relaunch the game**. Press **remove shader** to uninstall it.
   - The game must be **closed** when installing/removing the shader (the DLL is
     locked while running).

## How the shader works

The shader ships as a **DXGI proxy DLL**. `install shader` writes `dxgi.dll` into
the Ferns client folder (`%LocalAppData%\Ferns\Client\`), beside `Ferns.exe`.
On launch, the game loads our `dxgi.dll` first; it forwards every call to the
real system `dxgi.dll` and additionally hooks the swap chain to run a
post-process pass each frame.

The post-process reconstructs view-space position and normals from the game's
captured depth buffer, then ray-marches screen-space reflections and computes
ambient occlusion on top of the rendered frame. It's a pure screen-space effect
(no access to the engine's scene geometry), so it has the usual SSR limitations
(off-screen geometry can't reflect, etc.).

It does **not** modify any game files other than adding/removing this one
`dxgi.dll`.

## Building from source

Requires a **32-bit** MinGW-w64 toolchain (`i686-w64-mingw32-gcc` + `windres`)
on your `PATH` — 32-bit because `Ferns.exe` is a 32-bit process. On MSYS2:

```sh
pacman -S mingw-w64-i686-gcc
# then build from the MINGW32 shell, or add C:\msys64\mingw32\bin to PATH
```

Then just run:

```bat
build.bat
```

This compiles the shader (`shader/proxy.c` → `dxgi.dll`), embeds it into the
unlocker as a resource, and links `FernsUnlocker.exe`.

## Layout

| Path                 | What it is                                            |
|----------------------|-------------------------------------------------------|
| `unlocker.c`         | The unlocker GUI (Win32, C)                           |
| `unlocker.rc`        | Resource script — embeds the compiled shader DLL      |
| `shader/proxy.c`     | The DXGI proxy + SSR/SSAO post-process shader source  |
| `shader/proxy.def`   | DLL export table for the proxy                         |
| `build.bat`          | One-step build script                                  |

## Disclaimer

For educational use. Modifying a game client's memory and injecting DLLs may
violate the platform's terms of service — use at your own risk.
