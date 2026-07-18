# Hook Research: Borderlands GOTY Enhanced

## Validated Runtime Layout

The values below are runtime-discovered or stable RVAs for game version 1.5.0.0.

| Item | Address or offset | Validation |
|---|---:|---|
| `GNames` | game `+0x25D9740` | 73,315 entries, semantic names present |
| `GObjects` | game `+0x25D95D0` | About 205,000 UObject pointers |
| `FNameEntry` text | `+0x14` | Printable name consensus |
| `UObject::Outer` | `+0x40` | Reflection owner names |
| `UObject::Name` | `+0x48` | 128/128 samples, diverse indices |
| `UObject::Class` | `+0x50` | 128/128 metaclass validation |
| `UClass::SuperStruct` | `+0x78` | Class inheritance traversal |
| `UProperty::Offset_Internal` | `+0x8C` | Matches reflected fields |
| `UGameViewportClient::Draw` | game `+0x813860` | Runtime vtable hook heartbeat |
| Secondary viewport vtable | object `+0x60`, slot 2 | Validated executable entries |

The scanner does not rely on the fixed global RVAs. It scans writable PE sections and validates candidates against UE3 names and UObject structure.

## Camera Discovery

The standard `PlayerController::PlayerCamera` property is reflected at `+0x6D4`, but it is null during normal BL1 Enhanced gameplay. The game keeps its active view data directly in `WillowPlayerController`.

The scanner resolves these fields from `UProperty` metadata at runtime:

| Property | Current offset | Type |
|---|---:|---|
| `CalcViewLocation` | `+0xCA8` | `FVector` / three floats |
| `CalcViewRotation` | `+0xCB4` | `FRotator` / three UE3 integers |
| `CachedFOVAngle` | `+0xF48` | float |
| `LastCameraTimeStamp` | `+0xC7C` | float |

Example validated values:

```text
location=(-12247.2, 31618.3, 171.7)
rotation=(1440, -31572, 0)
fov=120.0
```

Class default objects named `Default__*` must never be accepted as live cameras. A previous heuristic accepted `Default__WillowPlayerCamera` and produced invalid data.

## Stereo Boundary

Runtime discovery finds the live `WillowGameViewportClient`, reads its secondary vtable at object `+0x60`, and hooks slot 2 (`Draw`). The hook is called once per normal game frame.

Calling `GameViewportClient::Draw` twice in one frame is unsafe in this build. It corrupts the UE3 heap and eventually terminates with `0xC0000374`. The hook must call the original function exactly once.

Validated stereo uses geometric AFR:

1. Select the render eye from the same frame counter used by Present capture.
2. Save `CalcViewLocation`, `CalcViewRotation`, and `CachedFOVAngle`.
3. Apply OpenXR head pose and the per-eye IPD offset.
4. Call `GameViewportClient::Draw` once.
5. Restore the original camera values.
6. Capture that frame into the matching eye texture during Present.

Runtime and headset validation confirmed geometric parallax and head tracking. A stability run exceeded 5,700 frames without errors.

## D3D11 and OpenXR

The standard temporary D3D11 device/swapchain technique now works when executed from the initialization thread outside `DllMain`. It provides the real DXGI and D3D11 method addresses.

MinHook activation is queued for all six hooks and applied atomically with `MH_ApplyQueued`. Enabling hooks individually caused an intermittent execute access violation while the render thread was active.

The game backbuffer does not consistently expose final gameplay composition. The current stable source is GDI window capture at 1920x1080, uploaded to D3D11 and submitted through OpenXR. This adds capture latency but preserves the final image.

## Remaining Work

1. Replace GDI capture with a reliable in-process final composition target to reduce latency.
2. Evaluate a safe inner scene boundary if same-frame stereo is still required. Never reenter `GameViewportClient::Draw`.
3. Remove the post-process convergence shift when a projection-matrix hook provides proper asymmetric per-eye projections.
4. Add motion-controller input and HUD depth handling.

## Diagnostic Tools

- `tools/inspect_ue3_camera_properties.py`: read-only live reflection inspector.
- `tools/scan_camera.py`: broad camera-pattern scanner; use only as a fallback.
- `tools/find_viewport_draw.py`: static viewport/vtable analysis.
