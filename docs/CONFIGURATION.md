# BL1 GOTY VR Configuration

## Installation

Place `dxgi.dll` and `BL1GOTYVR.dll` beside `BorderlandsGOTY.exe` in
`Binaries\Win64`. The DXGI proxy loads the mod automatically before the game's
first DXGI factory call; the injector is no longer required for normal use.

Only one `dxgi.dll` proxy can occupy the game directory. Remove or chain any
other DXGI-based overlay before installing this mod. Removing the proxy
`dxgi.dll` disables automatic loading without altering the game executable.

Run `BL1GOTYVRConfig.exe` from the same directory as `BL1GOTYVR.dll`. The
utility writes `BL1GOTYVR.ini`. Optical and tracking settings reload live after
Save; resolution and OpenXR refresh-rate changes require restarting the game.

| Setting | Range | Effect |
|---|---:|---|
| Render width/height | 640x480 to 7680x4320 | Game backbuffer resolution |
| Resolution scale | 0.50 to 2.00 | OpenXR eye swapchain scale |
| OpenXR refresh rate | 0 or 1 to 240 Hz | Requests the nearest runtime-supported rate; `0` keeps the runtime default |
| Camera FOV | 60 to 150 degrees | UE3 camera FOV during each eye render |
| IPD | 50 to 80 mm | Physical separation between eye cameras |
| Convergence shift | 0 to 20% per eye | Horizontal eye-image shift; `10` is the recommended GDI/AFR baseline and `0` keeps images parallel |
| Near/far plane | 0.01 to 100000 | OpenXR projection clipping range |
| Position scale | 0 to 5 | Head translation multiplier |
| Rotation scale | 0 to 5 | Head rotation multiplier |
| Same-frame stereo | Experimental | Forced off because re-entering `GameViewportClient::Draw` corrupts the UE3 heap |
| Reverse eyes | On/off | Swaps captured left and right textures |
| Camera roll | On/off | Enabled by default so roll orientation remains consistent with positional tracking |
| Debug logging | On/off | Enables runtime diagnostic logging |
| Aim dot | On/off | Shows or hides the controller-driven aiming dot |
| HMD-directed movement | On/off | Rotates left-stick locomotion by current HMD yaw instead of body/game forward |
| Physical crouch | On/off | Toggles the game's crouch when tracked head height crosses the calibrated posture threshold |
| Arm reach scale | 1.00 to 1.60 | Allows the IK arm segments to stretch toward the tracked hands; `1.35` matches a roughly 75 cm reach on the current rigs |
| Hide player body and arms | On/off | Experimental `Visibility/HidePlayerBodyAndArms` gate; off by default and only writes after runtime identity and reflected-schema validation |

## Render Presets

The configurator includes square render presets for common performance targets.
Selecting a preset fills only the width and height fields. The manually selected
OpenXR resolution scale is preserved. Click `Save settings` and restart the game
to apply the new game resolution.

| Preset | Game resolution |
|---|---:|
| Low | 1536x1536 |
| Medium | 2048x2048 |
| High | 2560x2560 |
| Ultra | 3072x3072 |
| Mega Ultra | 4096x4096 |

The DLL spoofs the primary display, monitor work area, system metrics, and game
client rectangle to the selected square resolution before UE3 builds its
viewport. This mirrors ME2VR's display pipeline and avoids desktop-height
clamping.

The configurator also updates `ResX` and `ResY` in the game's
`WillowEngine.ini` when it can locate that file. Otherwise, set the same
resolution in the game's video options.

Saving also sets `bWeaponBob=false` in `WillowGame.ini`, matching the official
Borderlands 2 VR configuration. This removes the native forward/backward walk
bob from the weapon and first-person arms so their motion comes from the tracked
controllers instead.

Physical melee uses the game's native hit authority while the IK suppresses its
viewmodel animation. With any character, hold the left trigger and swing the
left fist, or swing the right-hand weapon. During Brick's Berserk action skill,
either fist attacks directly without a trigger. Ordinary motion remains below
guarded speed, travel, front-of-head, and cooldown thresholds.

The left trigger does not activate ADS. Press it while the left hand is already
touching the active weapon to hold that contact point as a two-hand grip. Press
it with the hand away from the weapon to arm the left-hand physical melee.

## Runtime Diagnostics

| Key | Effect |
|---|---|
| `F8` | Cycles GDI, raw backbuffer, and tracked SDR capture sources |
| `F9` | Toggles experimental OpenXR FOV/crop correction |
| `F10` | Selects the next tracked SDR target |
| `F11` | Toggles GDI/AFR eye and rendered-pose latency compensation |

## Desktop Pose Simulation

`F6` enables pose simulation even when OpenXR or the headset is unavailable.
The simulated pose follows the same camera conversion path as a real HMD.

| Key | Simulated movement |
|---|---|
| Left/right arrows | Yaw |
| Up/down arrows | Pitch |
| `Page Up` / `Page Down` | Roll |
| Numpad `4` / `6` | Left/right |
| Numpad `8` / `2` | Forward/backward |
| Numpad `7` / `1` | Up/down |
| `Home` | Reset pose |

GDI capture, geometric AFR, and latency compensation are the stable defaults.
The other source/projection modes are retained only for runtime research.
