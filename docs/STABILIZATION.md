# Stabilization passes

## Current headset-validated reference (2026-11-12)

Source `55bb9f3a90cc` is the current combined reference. User confirms all four
weapons reload correctly, with no hand jitter or firing image/perspective break
in that run. Keep early post-animation stereo preparation, frozen on-foot
camera/recoil ownership, AND original native IK weapon-attachment ownership.
Do not reintroduce unconditional canonical component stamps once the engine's
IK attachment owns the weapon; magazine reads must remain enabled in that mode.
See `docs/RELOAD_V0573_COMPARISON.md` for the controlled original-DLL comparison
and the failed intermediate builds.

Installed DLL SHA256:
`2bf88aae7f66c7fb628b5bcb4f869aba1e2396875c8689a9e380eee2386e5460`.
It matches the isolated Release artifact; 32/32 CTest groups passed before
deployment. A DLL/manifest/INI/log baseline is preserved outside the game at
`C:/Users/Mastersell/AppData/Local/Temp/opencode/bl1-validated-55bb9f3a90cc-20261112-130450-454`.
Only documentation/backups were updated after successful headset feedback;
no further live configuration, DLL or source-code changes were made.
This validates the reported run, not all transitions, runtimes or long sessions.
Native reserve-ammo accounting and broader lifecycle work remain separate.

## Pass 1: implemented

- Remove the stray token that prevented `ArmIKSystem.cpp` from compiling.
- Separate the startup-validated native stereo state from reloadable INI settings.
  Neither saving unrelated settings nor changing the request switches renderer
  modes in the middle of a stereo pair. Mode changes require a restart.
- Infer the configurator field count and validate field metadata at compile time.
- Validate all fields before writing. Save through a sibling temporary file and
  atomic replacement; preserve unrecognized settings and weapon profiles, and
  report permission/lock failures without replacing the original INI.
- Respect the Post FX checkbox and put all five Post FX fields in its UI group.
- Default the configurator to alternate-eye stereo, matching the DLL defaults.
- Remove the hardcoded Visual Studio Community path from `build.bat` and return
  failures to its caller.
- Log version/Git/source identity and generate `build-info.json`. The fingerprint
  includes local project-source edits, not binaries or external dependency trees.

## Automated checks

Enable `BL1GOTYVR_BUILD_CORE_TESTS` and `BL1GOTYVR_BUILD_RENDER_TESTS` in CMake.
CTest exercises native stereo request/state separation, external INI reloads,
field parsing (including empty/nonfinite inputs), preset validation, atomic
saves, profile preservation, readonly/locked files, configuration clamps,
IK/room-scale self-tests and the existing 16 HUD rendering cases.

Tests own temporary files in the build directory. They do not load the mod DLL,
inject the game, write the game's configuration, or validate headset behavior.
Test executables are emitted under `tests/<configuration>`, away from `dxgi.dll`.
Do not copy the proxy/mod into that directory: a DXGI test beside the proxy can
load the mod and modify real game settings.

## Pass 2: controlled turning (pending in-game validation)

- Share snap/smooth/off foot-turn calculation between XInput and keyboard/mouse
  input paths. Use degrees, not arbitrary mouse pixels, for on-foot yaw.
- Queue controller yaw deltas to the game thread's eye-0 boundary so both eyes
  share one base rotation. Preserve fractional rotator units for low-speed turns.
- Require reflected, typed/aligned `Actor::Rotation` metadata and reflected
  world/pauser pointers. No yaw writes in a paused world, a recognized terminal,
  theater mode, the tuning overlay, or without a matching live player identity.
  Optional reflected cinematic/look-ignore bits add engine input guards.
- Retain native XInput steering/menu axes and physical-gamepad merging. If
  metadata is missing, retain native smooth XInput sensitivity and disable exact
  snap rather than guess a memory offset or mouse sensitivity.
- Require neutral to rearm snaps and after context/mode changes. Cancel stale or
  wrong-player queued turns and cap long-frame input debt.
- Add five turn test groups: frame-rate-independent smooth speed, snap gestures,
  context/mode safety, mailbox expiry/identity, and rotator wrap/quantization.
- Add live Turning controls/status to F10. Existing INI keys are unchanged.

These unit tests cannot prove native controller writes survive the game's next
tick or respect every inventory/cinematic state. Verify the `[Turn]` discovery
log and complete the in-game checklist in `CONFIGURATION.md` before deployment.

## Pass 3: walking hand/weapon coherence (pending headset validation)

- Publish the current pair's arm targets before native ViewportDraw, not after
  eye one. Reapply the hands then the weapon before Draw and command capture.
- Capture native animation before IK, and use that untouched input for each new
  pair. Do not solve a new target from the previous pair's stretched IK bones.
- Serialize pair/target context publication with post-animation/palette applies.
- Keep canonical weapon stamps active even when IK is ready: native attachment
  refreshes are not guaranteed to consume the post-animation wrist correction.
- Use the current right-hand target/barrel basis for off-hand weapon grabs,
  instead of the previous frame's world-space weapon frame.
- Invert component scale as well as rotation during target conversion and cached
  stereo replay; strip scale before extracting the component rotation.
- Add three pose-math test groups and source-level pipeline order guards. These
  guard known regressions, but do not execute UE3's attachment/render pipeline.

Validate walking/strafing with stationary controllers, then turns, two-hand grab,
weapon changes, reload/fingers, recenter, crouch and vehicles. Check the new
`[ArmIK] Current-pair render preparation` entries and weapon drift corrections.
No controller smoothing or tracking delay was added.

### Follow-up: left-eye ghosting only during walking

The first validation reported a stable right eye but residual left-eye shimmer
while walking (not while moving the controllers/head at rest). The final
`ApplyPoseToView` path still selected the native source view origin for ordinary
on-foot stereo, ignoring the frozen ticket anchor used by hands and weapon.
Real XR views now use that ticket anchor. Shader pre-view translation receives
the full native-to-final origin delta, not only the HMD/IPD offset. Vehicle
anchor selection and desktop/native fallback are retained. Per-eye
`[Camera] Stereo anchor` logs report source drift; a walking regression checks
hand-to-eye coordinates and consistency of full/translated origins.

This fixes a source-level inconsistency, not proof of the reported symptom's
cause. The current log cannot establish GPU skinning upload timing or compositor
reprojection. Full REA analysis of the shipped executable timed out; bounded
read-only disassembly of the running process did not establish an upload hook,
so no new native call/offset or live patch was introduced. In-headset validation
of both eyes is still required.

### Follow-up: prepare the first eye during native animation

Headset validation of `765389269853` was unchanged: right eye stable, left eye
unstable while walking. Its per-eye native anchor differences were at most
about 0.002 UU in the sampled entries, so anchor drift is not a supported
explanation for that session.

The previous path still let the native animation hook consume the last pair's
context before Draw published the next one. The new path prepares an immutable
eye-zero ticket immediately after native arm animation and before post-animation
IK, then lets Draw and eye one reuse it. Repeated eye-zero acquisitions no longer
replace a ticket already sampled by animation. Both camera anchor and hand
targets are reused, not recomputed from a later walking position.

Early preparation requires the known viewport/game thread, a stable armed player
identity, valid tracking/calibration, and an ordinary on-foot context. Worker
threads, startup, weapon/pawn changes, recenter, vehicles, injury and transient
camera actions retain the Draw fallback. Preparation cannot reenter during Draw.
An invalidated early pose forces a palette rebuild without resetting wrist or
weapon calibration. No new native offset/call, eye-order swap, extra Draw or
controller smoothing is used.

New logs distinguish `Pair prepared post-animation`, `Post-animation pose`, and
per-eye Draw preparation (`preparedAtAnimation`, `solvedPair`). During ordinary
walking, check that early preparation is active and that both Draw eyes report
the intended pair. Two portable timing test groups cover repeated acquisition,
pair completion/abort and stale/late staging; source guards cover call order.
These tests do not observe GPU upload/compositor timing. This remains a targeted
validation build, not a confirmed fix for the headset symptom.

Headset feedback for source `478e6f0a7e55`: both eyes were stable during that
run. Preserve its early-preparation timing while investigating hand alignment.
The user reports an equal upward positional displacement of both hands, unchanged
by recenter. Global hand translation settings are zero; magnitude and
wrist-versus-grip-center alignment still need to be established. The installed
F10 `Hand position` controls can compensate this with equal negative `Up` values
for Left and Right, without replacing the stable DLL or changing stereo timing.
Close F10 before assessing walking stability; early preparation intentionally
uses the fallback while the overlay is visible. This report
does not yet validate long sessions, all transitions or every stereo mode.

### Follow-up: firing changes the camera perspective

The same `478e6f0a7e55` run also had a user-reported image/perspective break on
firing. Its log shows recoil correction beginning with the first recorded shot,
and later a first-person reference reset followed by pitch `-5817` (about -32
degrees). These observations do not locate the visual failure to a particular
render command or prove a single cause. Do not compensate hand height in the
INI while investigating the camera reference.

Source review found that InputHook's UpdateState (Present/XInput callers) wrote
the camera cache through SuppressRecoilTick, concurrently with viewport camera
save/restore. Its unprotected recoil-anchor array could also sample a transient
VR-modified rotation. Input now only publishes atomic fire state; correction is
guarded to the discovered viewport thread, with player identity and on-foot
checks, a per-identity 400 ms release hold, and the trusted base pitch. It never
pins yaw/roll or introduces a new native offset.

On-foot reference capture uses neutral game pitch, leaving head pitch to the
HMD. This prevents an unarmed reset from treating restored HMD interaction pitch
as a new permanent camera base. Invalid startup native pitch is rejected; valid
non-foot native pitch is normalized and retained. The final on-foot XR view
uses the frozen ticket rotation rather than composing native firing shake or
interaction rotation with the HMD again. Native/menu/vehicle basis selection and
the existing first-person recovery path are retained. Early arm preparation and
stereo eye order are unchanged.

Five new policy test groups cover reference resets, recoil release/identity,
thread ownership, basis math and stereo/native-source isolation, plus source
guards. They do not execute native shooting or demonstrate the in-headset fix.
Validate single shots, held fire, releasing the trigger, head movement/turning
while firing, unarmed/weapon transitions, recenter and vehicles. Inspect
`Game pitch reference`, `Recoil pitch corrected on viewport thread` and per-eye
`Frozen firing view` logs. Preserve the previous validation DLL for comparison.

### Follow-up: manual reload acquisition and invisible magazines

Headset feedback for source `446e897f45e7`: the firing image/perspective failure
did not recur and hand positions were correct. Preserve its camera policy,
early post-animation preparation, eye order and hand/weapon calibration.
The same run still had intermittent reload highlighting/acquisition and invisible
extracted magazines. Its log records three complete logical extraction/pouch/
insertion sequences (combat rifle, repeater, repeater); this is not evidence of
visible magazine rendering.

Source review found a fresh-LT-edge requirement inside the extraction zone,
and magazine conversion/writes before tracked weapon restamping. The latter
could compute local coordinates against a native matrix, then move the magazine
again when the tracked weapon matrix was written. Live XR/camera samples and a
world-fixed well anchor also disagreed with the visible frozen weapon/hand.

The validation change allows held LT to acquire upon entering the zone, uses
controller grip position (not aim-ray origin), keeps the released-magazine cue
active, and recomputes the seated well from the current tracked weapon matrix.
Its existing body offset follows weapon up. It publishes the tracking-to-world
frame alongside the pair's arm targets and copies that frame with the driven
weapon pose; it does not change arm target math or camera preparation timing.
The highlight's asymmetric-FOV vertical projection is corrected, and its
position/radius/active flag are published as one locked snapshot.

Magazine intent is published by input; seating capture/restoration and carried
pose writes run on the weapon-preparation thread. The seated cache is scoped to
weapon, component, mesh, palette allocation, bone and bone count. Release waits
for a valid owned seated pose rather than vibrating before any visual pose is
available. Carry uses the visible left palm/wrist frame, no extra upward trim,
and a full affine component inverse. Both restamp entry points place the weapon
first, then the magazine. Weapon swaps reset the logical sequence. No ammo
transfer policy, INI/profile, native offset or GPU upload hook is changed.

Release build and 30/30 CTest groups pass, including five new pure reload groups
and source ownership/order guards. Tests remain isolated from the DXGI proxy;
five existing protected mod/game settings files were hash-verified unchanged.
Tests do not observe native GPU skinning, culling, magazine visibility or actual
controller interaction. In-headset validation remains required.

Deployed validation source `d4f95ff2e0cf` to the game's `Binaries/Win64`, replacing
only the DLL and build manifest after checking game/configurator/injector closed.
SHA256 verification passed; previous validation and log preserved in
`vr-backup-20261111-133504-372-manual-reload-d4f95ff2e0cf` at that destination.

Validation: hold X for at least 350 ms, approach the released magazine with LT
already held, carry/rotate it, release LT to discard it, acquire a fresh one at
the left hip with LT, and release LT at the moving/tilted weapon to insert.
Repeat with combat rifle and repeater, then cancel with X and switch weapons.
Confirm both eyes, walking, hand alignment and firing stay stable. Inspect
`Seated magazine cached`, `Release deferred`, `Magazine visual deferred`, and
`Magazine pose written` (CPU world-position error only, not GPU proof).

### Validation failure: stale equip pose and mismatched cue projection

Headset feedback for `d4f95ff2e0cf`: reload still failed on most weapons; some
showed cues at unrelated points or none, and only the shotgun could extract,
with its cue floating to the left. Do not regard that build as a reload fix.
The log detects named magazine bones on combat rifle (1/4), sniper (11/14),
repeater (1/7), and shotgun (25/27). Rifle acquisition attempts stay about
0.4–0.5 m from the computed well. Only shotgun mode 1/2 pose writes occur.
The log therefore does not establish incorrect bone-name selection or native
GPU visibility as the cause of this particular failure.

Source review found that the previous cache froze the first valid matrix on
equip, even while equip animation was still moving the mag. Idle now refreshes
the owned seated reference; release freezes it explicitly (mode 4) and reapplies
that same seated matrix to the visible magazine. Restoration cannot recapture
the pre-restoration carried matrix, and revolver spin cannot poison the cache.
The family-independent 8 cm body offset is removed. This addresses concrete
source faults but does not prove that every bone pivot is the mesh's grip center.

The previous cue helper also projected directly into asymmetric OpenXR FOV,
despite the game rendering a symmetric source and GetProjectionCrop intentionally
centering the vertical horizon. That earlier claimed correction was wrong for
this pipeline. The helper now projects with measured source half-tangents and
inverts the final actual blit scale/offset, including containment, latched crop,
convergence and SBS selection. It changes only the cue, not camera projection,
crop policy, arm IK, eye timing, mount calibration or saved settings. Missing
source measurements suppress the cue rather than guessing a projection.

Validation source `eb13703b85c0`: Release compilation and 31/31 CTest groups
pass. Tests now include equip-to-settled cache refresh, release freeze, restoration
and spin isolation, and projection round-trips through actual sampling UVs.
Five protected settings files remain unchanged by SHA256. These tests do not
execute the native weapon animations or prove an in-headset result. New
`Release reference` logs record local bone position/cache age; `Cue projection`
records visibility, measured-source status, stage/local positions and output UV.
Next headset check should start with rifle and shotgun, checking cue alignment,
extraction, carried visibility and cancellation before a full pouch reload.
Deployed only DLL/manifest to `Binaries/Win64` with SHA256 verification and
game/configurator/injector closed; backup:
`vr-backup-20261111-134615-315-reload-reference-eb13703b85c0`.

## Next stages

1. Validate native turning in-game: angle/speed, next-tick persistence, movement,
   paired eyes, pause/inventory, cutscenes, vehicles and multiplayer.
2. Make manual reload use native ammo transfer/reserve accounting. Validate empty
   reserve, partial clips, weapon switching and network authority before changing
   live ammo offsets.
3. Complete swapchain/overlay resource teardown before resize and review hook
   chaining instead of restoring other overlays' entry patches.
4. Validate SteamVR and VDXR with sustained sessions, map transitions, recenter,
   tracking loss, focus changes and both stereo modes.

Existing local weapon-pose freshness changes (250 ms to 1000 ms) are preserved;
this pass does not claim they have been validated in-headset.
