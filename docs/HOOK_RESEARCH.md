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

### Coherent AER Reimplementation (2026-07-30)

The active stereo path now follows the validated Mass Effect 2 VR pair protocol rather than the later experimental native-multiview path:

1. Eye 0 snapshots the OpenXR head pose, both runtime eye positions/FOVs, and the unmodified BL1 camera state.
2. Eye 1 restores that same BL1 camera state and reuses the exact eye-0 OpenXR snapshot.
3. Each completed Draw publishes a `{pairSerial, eye}` ticket to the pre-Present capture path.
4. Eye textures are submitted only when both captures carry the same serial.
5. The exact rendered `XrView` pair is used for submission; projection crop uses the same frozen FOV and render aspect as the camera.

IPD comes from the OpenXR runtime in headset mode. The configured IPD remains only as the desktop-simulation fallback. `RenderScene`, render-command multiview, and double-Draw hooks are not installed, so camera pose and projection are applied at one authority only: the validated `WillowPlayerController` camera cache around a single `GameViewportClient::Draw` call.

## D3D11 and OpenXR

The standard temporary D3D11 device/swapchain technique now works when executed from the initialization thread outside `DllMain`. It provides the real DXGI and D3D11 method addresses.

MinHook activation is queued for all six hooks and applied atomically with `MH_ApplyQueued`. Enabling hooks individually caused an intermittent execute access violation while the render thread was active.

RenderDoc capture `capturas-frame5917.rdc` confirms that the complete frame, including `Post-PostProcessRendering` and `FGFxEngine::RenderUI`, finishes in the SDR swapchain backbuffer (`R8G8B8A8_UNORM`, captured resource 11745 / RTV 11756). The default path temporarily unbinds this RTV, copies it to the AFR eye texture, and restores the render targets before `Present`. Copying while it remained bound could silently preserve an old loading frame. HDR resources are not observed or submitted. `F8` can switch diagnostically between direct SDR backbuffer, tracked SDR targets, and GDI.

The renderer view object is reachable through `renderer+0x68`. Its camera origin is currently at view `+0x2C0`, and a validated inverse projection matrix starts at view `+0x240`. A reciprocal projection-like matrix was found at view `+0xC0`, but writing asymmetric offsets there removed the world render while leaving HUD composition active. Engine projection writes are disabled; `F9` only controls the non-destructive submission crop.

The `RenderScene` call stack shows that `FSceneView` belongs to a render-thread command (`0x1F7CB0 -> 0x436A19 -> 0x435ECE -> 0x4578BC -> 0x46D77F -> 0x468220`). Restoring its matrices after `RenderScene` caused a general protection fault because the command may destroy the view before returning. The command contains an array at `renderer+0x68`, a count at `+0x70`, and a view stride of `0x1750`. `RenderScene+0x2A7` copies the active matrices into private backup fields beginning at view `+0x590`, then restores them for later render phases. Phase 2 has another restore path guarded by view `+0x448`, sourcing ViewProjection matrices from `+0x490`, `+0x4D0`, `+0x510`, and `+0x550`; these must also receive the tracked pose. The experimental path writes every validated principal view and both restore caches before `RenderScene`, then never accesses the command after the call.

## Remaining Work

1. Validate visible side-by-side output from the two native command views.
2. Route each same-frame view into its matching OpenXR eye texture.
3. Locate projection creation and frustum construction for asymmetric per-eye projection.
4. Add motion-controller input and HUD depth handling.

## Historical Native Multiview Stereo Progress (2026-07-19, inactive)

Same-frame geometric stereo now uses UE3's native multi-view command construction instead of re-entering `GameViewportClient::Draw`.

Validated command layout:

| Item | Location | Runtime result |
|---|---:|---|
| Render command constructor | RVA `0x445280` | Producer/game thread |
| Render command execution | RVA `0x46D630` | Render thread |
| Copied `FSceneViewFamily::Views` | command `+0x08` | `TArray<FSceneView*>` |
| Owned view copies | command `+0x68` | `TArray<FSceneView>` |
| View count/capacity | command `+0x70/+0x74` | Normal frame `1/1` |
| Owned view stride | `0x1750` | Confirmed in constructor, renderer, and destructor |
| Native view copy constructor | RVA `0x447150` | Non-trivial; never replace with `memcpy` for owned views |
| Command destructor | RVA `0x44A9A0` | Destroys every owned view after `RenderScene` |

The normal command has no spare capacity, so changing only `Num` from one to two is unsafe. The implemented approach supplies the original command constructor with a temporary source family containing two principal-view pointers. UE3 then performs all allocation, copy construction, resource registration, family pointer repair, aggregate calculations, and destruction itself.

Runtime validation reached more than 3,900 frames with:

```text
family num/max=2/2
owned num/max=2/2
view[0].Family == command+0x08
view[1].Family == command+0x08
family.Views[0] == owned view[0]
family.Views[1] == owned view[1]
```

No exception, heap corruption, or process hang occurred. This validates native multi-view lifetime and traversal. The next build creates two temporary source views with half-width rectangles and applies opposite IPD offsets to the two owned command views. Its purpose is to validate visible side-by-side geometric parallax without a headset.

Current continuation point:

1. Run `build/Release/injector.exe` and confirm the desktop backbuffer contains left/right half-width views.
2. Read `[StereoResearch]` logs and confirm both constructed rectangles and `Final command view pose applied ... views=2/2`.
3. Verify parallax using the simulated pose controls (`F6`, arrows, `Page Up/Page Down`, numpad).
4. Split the side-by-side source in `FrameLoop` and copy each half into its matching OpenXR eye texture in one frame.
5. Replace symmetric FOV plus image shift with OpenXR asymmetric per-eye projections and rebuild frustum planes before culling.
6. Keep AFR only as a fallback; same-frame multiview is the target architecture.

The main BasePass is part of `RenderScene`'s four-phase loop. Lower candidates such as RVA `0x46D060`, `0x4817F0`, and `0x4821C0` are only prepass, target binding, or clear operations and are not complete replay boundaries. Native multi-view avoids replaying those mutable phases manually.

## Diagnostic Tools

- `tools/inspect_ue3_camera_properties.py`: read-only live reflection inspector.
- `tools/scan_camera.py`: broad camera-pattern scanner; use only as a fallback.
- `tools/find_viewport_draw.py`: static viewport/vtable analysis.
