# AGENTS.md — BL1 GOTY VR

VR mod for Borderlands: Game of the Year Edition Enhanced (UE3, D3D11, x64),
targeting Quest 3 through SteamVR/OpenXR.

## Build

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

Outputs (copy to `<game>\Binaries\Win64\`):

| Artifact | Purpose |
|---|---|
| `BL1GOTYVR.dll` | the mod (loaded by the proxy or the injector) |
| `dxgi.dll` | DXGI proxy: forwards to System32, optionally chains `mods\` post-processing proxies, auto-loads the mod |
| `BL1GOTYVRConfig.exe` | Win32 configurator that edits `BL1GOTYVR.ini` |
| `BL1GOTYVR_ReShadeCapture.dll` | ReShade add-on (deploy as `*.addon` next to ReShade) |

**Always do a clean rebuild after changing a header that defines structs**
(`Config.hpp` most notably):

```powershell
cmake --build build --config Release --clean-first
```

MSBuild does not reliably notice new/removed struct fields, so stale `.obj`
files keep the old layout and read every field at the wrong offset. Symptoms
are "impossible" values at runtime (e.g. `cut=10.2` or `dot=0.00` while the
INI says 70/5), misaligned weapons and broken aim/dot. A clean rebuild fixes
all of them at once.

## Runtime configuration

`BL1GOTYVR.ini` (next to the game exe). The mod reloads it when the file
changes, so configurator edits apply live. Keys are grouped by section:
`[Display] [Stereo] [Rendering] [Tracking] [RoomScale] [IK] [Debug]
[Visibility] [Weapon] [MotionControls] [Dot] [Melee] [Hands] [HUD] [PostFx]`.

Native post-processing is applied per eye in the blit shader (`[PostFx]`):
`Enabled`, `Sharpen`, `Contrast`, `Saturation`, `Brightness`, `Gamma` and
`FsrUpscale` (FidelityFX FSR1 EASU, used only when upscaling).

## Verified don'ts

- Do not leave `d3d11.dll` in the game folder (breaks the OpenXR compositor).
- Do not deploy the DLL while `BorderlandsGOTY.exe` is running.
- Do not change `Config.hpp` fields without a clean rebuild (see above).
- Do not compute the aim dot from cached ticket data without validating it;
  `[DotGuard]` rebuilds an aim point that arrives without its distance offset.

## Debugging

`BL1GOTYVR.log` next to the game exe. Useful one-shot probes: `[Config] Loaded`
(prints the loaded values, including `cut=`), `[VanillaHands]` (geometry cut),
`[DotSource]`/`[AimDotSource]`/`[ReticleProbe]` (aim dot), `[PostFx]` (blit
post-processing), `[PresentChain]` (who owns the swapchain Present).
`BL1GOTYVR.proxy.log` records the DXGI chain decision made by `dxgi.dll`.
