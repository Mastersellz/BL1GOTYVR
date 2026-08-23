# Borderlands GOTY Enhanced VR

Native OpenXR VR mod for **Borderlands: Game of the Year Enhanced** on Windows.
It adds stereoscopic rendering, headset 6DoF, motion-controller aiming, native
gamepad input, a VR HUD, and configurable render settings without modifying the
game executable.

> [!IMPORTANT]
> This project targets **Borderlands GOTY Enhanced (2019), Win64/D3D11**. It is
> not compatible with the original 2009 release.

## Current Status

The main gameplay path is working and headset-tested:

- OpenXR output through SteamVR/Steam Link and VDXR/Virtual Desktop
- Geometric stereo rendering
- Stable rotational and positional 6DoF
- Resolution-independent projection and compositor FOV matching
- Quest Touch controller input through an XInput bridge
- Controller-directed weapon and ballistic aim
- VR crosshair and HUD handling
- Configurable square render resolutions from 1536x1536 to 4096x4096
- Automatic loading through a `dxgi.dll` proxy

First-person arm IK, body hiding, alternative capture sources, and same-frame
stereo remain experimental. The stable stereo path uses synchronized alternate
eye rendering.

## Requirements

- Windows 10 or 11, 64-bit
- Borderlands GOTY Enhanced
- A working OpenXR runtime
- An OpenXR-compatible headset; development and validation target Quest 3
- Visual Studio with the Desktop development with C++ workload, for source builds
- CMake 3.24 or newer, for source builds

Dependencies are fetched automatically by CMake:

- OpenXR SDK 1.1.58
- MinHook 1.3.4

## Installation

Copy these files from a Release build beside `BorderlandsGOTY.exe`:

```text
BorderlandsGOTYEnhanced\Binaries\Win64\
|- BorderlandsGOTY.exe
|- BL1GOTYVR.dll
|- dxgi.dll
`- BL1GOTYVRConfig.exe   (optional)
```

The DXGI proxy forwards calls to the real system DXGI library and loads
`BL1GOTYVR.dll` before the game creates its first graphics factory.

Only one `dxgi.dll` proxy can occupy the game directory. Remove or chain any
other DXGI proxy before installing the mod. To disable automatic loading,
remove this mod's `dxgi.dll`; the game executable is never patched.

Select and start the desired OpenXR runtime before launching the game. The mod
respects `XR_RUNTIME_JSON` when supplied; otherwise it uses the system
`ActiveRuntime` selection, including SteamVR and VDXR.

## Configuration

Run `BL1GOTYVRConfig.exe` from the game directory. It creates
`BL1GOTYVR.ini` beside the DLL. Most optical and tracking options reload when
settings are saved; changing the render resolution requires restarting the
game.

Recommended starting presets:

| Preset | Game resolution | OpenXR scale |
|---|---:|---:|
| Low | 1536x1536 | 0.75 |
| Medium | 2048x2048 | 1.00 |
| High | 2560x2560 | 1.25 |
| Ultra | 3072x3072 | 1.40 |
| Mega Ultra | 4096x4096 | 1.50 |

Start with **Medium** and increase the resolution only if GPU headroom allows.
The configurator also attempts to update `ResX` and `ResY` in the game's
`WillowEngine.ini`.

See [docs/CONFIGURATION.md](docs/CONFIGURATION.md) for all settings, live
reload behavior, diagnostic keys, and desktop pose simulation.

## Controls

The mod exposes the VR controllers to the game as an XInput controller. Game
bindings therefore continue to determine the final action assigned to each
button.

| Quest control | Default behavior |
|---|---|
| Left thumbstick | Move |
| Right thumbstick | Smooth or snap turn |
| Right trigger | Fire |
| Left trigger | Aim down sights |
| A | Jump |
| B | Crouch |
| X | Use/reload game action |
| Left grip | Left shoulder action |
| Right grip | Right shoulder action |
| Y tap | Cycle weapon |
| Y hold | Echo/menu back action |
| Y hold + left thumbstick | D-pad/weapon slot selection |
| Left menu button | Start/pause |
| L3 + R3 | Recenter headset and aim reference |

The right controller pose drives weapon direction and the guarded ballistic aim
override so the VR marker and shot trace remain aligned.

## Building

From a Visual Studio developer command prompt:

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

Or run:

```bat
build.bat
```

Release outputs are generated under `build\Release\`:

- `BL1GOTYVR.dll`: main mod
- `dxgi.dll`: automatic loader proxy
- `BL1GOTYVRConfig.exe`: configuration utility
- `injector.exe`: optional development loader

The target must remain **x64** because `BorderlandsGOTY.exe` is a 64-bit
process.

## Diagnostics

Runtime diagnostics are written to `BL1GOTYVR.log` beside the game executable.
Useful development keys include:

| Key | Action |
|---|---|
| F6 | Toggle desktop pose simulation |
| F8 | Cycle capture source |
| F9 | Toggle projection crop correction |
| F10 | Select the next tracked SDR target |
| F11 | Toggle capture/pose latency compensation |

These keys are intended for troubleshooting. The default capture and projection
settings are the validated path.

## Architecture

The mod combines several interception layers:

- DXGI proxy loading and D3D11 swap-chain hooks
- UE3 camera and `FSceneView` discovery
- Per-eye camera pose and projection injection
- Pair-paced eye capture and OpenXR composition
- Runtime-specific swapchain format handling
- XInput interception for native controller integration
- HUD extraction or integrated projection, depending on the runtime

The final OpenXR projection layer submits the exact pose and centered vertical
FOV used to render each captured eye. Keeping the rendered rays and compositor
metadata identical is required to prevent world movement during head rotation.

## Known Limitations

- Same-frame re-entry into `GameViewportClient::Draw` is disabled because it
  corrupts the UE3 heap in this game build.
- First-person arm IK and body visibility changes are experimental and guarded.
- Another mod using `dxgi.dll` requires proxy chaining or an alternative loader.
- Game updates may invalidate signatures or reflected UE3 offsets.

## Uninstall

Remove the following files from `Binaries\Win64`:

```text
BL1GOTYVR.dll
dxgi.dll
BL1GOTYVRConfig.exe
BL1GOTYVR.ini
BL1GOTYVR.log
```

If another mod previously supplied `dxgi.dll`, restore that file afterward.

## Disclaimer

This is an unofficial fan project. Borderlands and related trademarks are the
property of their respective owners. Use the mod at your own risk.
