# BL1 GOTY VR Configuration

Run `BL1GOTYVRConfig.exe` from the same directory as `BL1GOTYVR.dll`. The
utility writes `BL1GOTYVR.ini`. Optical and tracking settings reload live after
Save; only resolution changes require restarting the game.

| Setting | Range | Effect |
|---|---:|---|
| Render width/height | 640x480 to 7680x4320 | Game backbuffer resolution |
| Resolution scale | 0.50 to 2.00 | OpenXR eye swapchain scale |
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

The configurator also updates `ResX` and `ResY` in the game's
`WillowEngine.ini` when it can locate that file. Otherwise, set the same
resolution in the game's video options.

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
