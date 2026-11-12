# Original v0.5.7.3 reload comparison

## Reference and boundary

User reports reload worked in the original v0.5.7.3, but still fails in
`eb13703b85c0`. The original candidate found locally is
`Binaries/Win64/BL1GOTYVR V0.5.7.3.zip` (SHA256
`3bfbe6ae1fba455e1fa0fe3bbbb9daeec67b181aa3bbb1a1da760cd46d8e7dce`).
REA extracted it without replacing game files. Its DLL is 1,501,184 bytes,
SHA256 `02a9c77c6a46ea01144eca6e78032b35b8a7716fe9201a93852ed0b070b0b440`.
The repository's existing `build/Release/BL1GOTYVR.dll` has a different SHA256
(`507a46dddf447cc34751a7cd59642935d6e1bb2266da58b03fc6398cffe72d74`),
so it must not be substituted for the archived original.

This was static inspection of the archived DLL, not execution or a headset test.
Recovered pseudocode is not original source. The inspected routines resemble
the repository HEAD implementation, but whole-build source equivalence is not
established. No game/configurator was launched or terminated; no DLL, INI,
profiles, loader or game settings were replaced during this comparison.

## Observed differences

1. Original reload sequencer (`0x180038a60`) uses off-hand aim position and a
   fresh trigger edge in its extraction/pouch zones. Current reload uses grip
   position and permits held-trigger acquisition. The original includes the
   8 cm stage-vertical offset and fallback grip-space well estimate. It latches
   a stage-space insertion point after extraction.
2. Original carried-magazine routine (`0x180042990`) reads the live native
   component matrix, obtains a live XR hand rotation and camera reference,
   prefers the visible palm position when available, and adds 3 UU upward trim.
   Current code uses the tracked component snapshot and visible wrist rotation,
   with no trim, an idle-updated seated cache and a released/seated mode 4.
3. Original reapplication (`0x1800438c0`) invokes the magazine routine BEFORE
   checking whether a weapon matrix stamp is possible. Its native animation
   hook (`0x1800601d0`) invokes magazine handling even for components that are
   not the selected weapon. Current code invokes magazine handling AFTER a
   successful weapon stamp and rejects non-weapon-preparation threads.
   These are materially different native callback/pose-update schedules.
4. The archived and current INIs have 1,315 shared keys; only refresh rate
   (90 vs 72 Hz) and the two global active-weapon aim values differ. The shared
   per-weapon profile/mount values match. The current file has one extra key.
   This does not prove settings irrelevant, but does not support replacing
   calibration profiles wholesale as a reload fix.

The scheduling difference is a hypothesis for the regression, NOT a demonstrated
native skinning/upload cause. It also cannot alone explain the user's earlier
failure in `446e897f45e7`, before the newer callback restrictions were introduced.
Pure math/source-order tests did not exercise the original native update schedule
and must not be represented as proof of working reload rendering.

## Evidence

- Archive extraction: `ev_41f7fa028e450ed564f52d78e93b54c434d37e7aef23d3b541596fb30c392c42`.
- String inventory: `ev_70c39e40945d84ae397c9faf38b401b421a4a5818c27cdc70f34d39b14de3483`.
- Sequencer/carried pose pseudocode:
  `ev_28058e2e9a3a94b264c13c6b7c617ee25282dc23a54e03246b17676b1f1fad91`.
- Reapplication/native animation hook pseudocode:
  `ev_2698584181a2dfb66f225902085a2b855cf2d667519d72637cd41ea87922c1ac`.

Evidence export and closed-session analysis snapshot are under
`C:/Users/Mastersell/AppData/Local/Temp/opencode/` as
`bl1-v0573-original-reload-evidence.json` and
`bl1-v0573-original-analysis.json`.

## Controlled next step

Establish the archived DLL as the known-good reference with the CURRENT INI,
not the archive's INI. A temporary original-DLL-only A/B requires user approval:
it can bring back the original camera/hand instability and firing image break.
Preserve the installed DLL/manifest/log first and restore the stable validation
after the comparison. Do not deploy another speculative combined build or erase
the verified camera/hand fixes merely to restore the old reload schedule.

## Approved original-DLL A/B deployment (2026-11-12)

User approved the temporary original-only test. The first process check found
the game running, so no replacement was performed then. After the user asked
to continue, game/configurator/injector were confirmed closed. Only
`Binaries/Win64/BL1GOTYVR.dll` was atomically replaced from the verified original
ZIP. Installed SHA256 is
`02a9c77c6a46ea01144eca6e78032b35b8a7716fe9201a93852ed0b070b0b440`.
The previous `eb13703b85c0` DLL, existing log, INI/profiles, manifest, loader and
configurator were backed up under the game directory:
`vr-backup-20261112-114611-078-original-v0573-ab`.

INI/profiles/log/loader/configurator and four existing game configuration files
were verified unchanged by SHA256. The existing build-info JSON was deliberately
left untouched to honor the DLL-only test; it describes the backed-up validation
build, NOT the currently active original DLL. The backup's `deployment.json`
records that distinction and both DLL hashes. No game was launched/terminated.
The original sequencer requires a fresh LT press after entering the
zone, not simply holding LT while approaching. Original camera/firing/hand
regressions can return during this temporary test.

## Confirmed four-weapon reload baseline (2026-11-12)

User tested all four weapons and reports they all worked correctly with the
archived original DLL. The installed DLL SHA256 was rechecked and still matches
the original. This establishes the user's known-good reload reference, not the
cause of the regression or overall camera/hand stability in that same run.
The test used the existing configuration, not the archive's replacement INI.

The log independently records four completed logical sequences:

| Weapon | Grab distance | Native clip refill |
| --- | --- | --- |
| Combat rifle | 0.15 m | 29 -> 36 |
| Sniper | 0.11 m | 7 -> 9 |
| Repeater pistol | 0.09 m | 6 -> 14 |
| Combat shotgun | 0.11 m | 7 -> 9 |

Each includes extraction, discard, fresh magazine acquisition and insertion.
Logical log transitions alone do not prove rendered visibility; the user's
headset feedback supplies the visual confirmation. Native reserve-ammo
accounting remains unvalidated by this test.

Preserved log:
`C:/Users/Mastersell/AppData/Local/Temp/opencode/BL1GOTYVR-original-four-weapons-20261112-121411-417.log`,
SHA256 `023806021195674f5c2eb8266e511174de25b813ea38c4c067b2270c4e3b4b21`.
Leave the original DLL installed for now. Before preparing a combined validation,
ask whether the same original-DLL run also kept hands/camera stable while walking
and firing. Do not assume those older symptoms returned or were fixed merely
from the successful reload report.

## Combined validation (2026-11-12)

User confirms the old hand/camera/firing failures DID return in the original-DLL
test. There are now two distinct headset-tested references: original DLL for
four-weapon reload, and `446e897f45e7` for stable hands/camera/firing. Neither
alone establishes that combining their paths works.

Prepared source `7c47d19124cd` with the original reload behavior restored:
fresh LT edges and aim-space distances; original bone/fallback well and latched
insertion anchor; capture-on-grab XR hand rotation; optional visible-palm position
with XR position fallback; original palm trim and rigid conversion; native
resident component reads; and magazine callbacks before component filtering and
weapon stamping, not only after successful stamps on the viewport thread.
Indicator math matches the original centered-vertical/per-destination-eye path.
The failed idle-updated/released-mode-4 path is removed from production.

The integration is not a byte-for-byte original module transplant: it retains
validated component/mesh/palette ownership checks, weapon-change reset and locked
highlight publication. A magazine-pose lock serializes capture/restoration and
the restored multi-thread native callbacks rather than excluding them. These
changes do not mutate camera state. Native ammo transfer policy is unchanged.

Early post-animation pair preparation, frozen on-foot camera rotation and recoil
ownership remain untouched. Six camera/hand/timing source files were hash-checked
unchanged during this integration. Only the reload indicator block changes in
FrameLoop, not pair scheduling or source camera/crop policy. Five protected
settings files remain unchanged through testing.

Release compilation and 31/31 CTest groups pass. Reload source guards now check
the original callback/interaction contract, not the failed experimental ordering.
Projection and capture tests likewise match the original behavior. Affine and
tracking helper tests remain arithmetic regressions, not tests of the restored
native callback path. No test executes engine animation, GPU skinning/upload,
actual controller input or compositor visibility. In-headset combined validation
is still pending: four-weapon reload and carried visibility, then walking in both
eyes and sustained fire/release/head movement without camera corruption.

Installed `7c47d19124cd` with game/configurator/injector closed, replacing only
DLL and manifest and verifying SHA256. The successful original DLL and its log
are backed up under `Binaries/Win64/` in
`vr-backup-20261112-122827-898-original-reload-integration-7c47d19124cd`.
INI/profiles/loader/configurator/game settings were preserved; no game launched.
The installed manifest once again describes the actual installed DLL rather
than the temporary original-only A/B reference.

## Integration failure and missing weapon-owner branch

User reports `7c47d19124cd` still cannot extract magazines; shotgun cue remains
displaced. This build is NOT a validated combined fix. A bounded read-only log
snapshot was preserved while the game remained running:
`C:/Users/Mastersell/AppData/Local/Temp/opencode/BL1GOTYVR-failed-7c47-20261112-123735-416.log`,
SHA256 `3f8ecb52426b46a57cf418db27f3e0886fe233be797cea8a42eef38219445db7`.
It contains releases/cancellations and some nearby-cue projections, but no
`Grip`/extraction transitions. All 24 periodic tracked-input samples have LT=0;
sampling is sparse, so this does not prove a trigger defect or user error.

The previous integration restored reload code but missed the ORIGINAL weapon
placement owner policy. Reinspection of the original archived DLL's
`FUN_18003f200` observes a latched flag at `this+0x6e2`: after the two rig-status
flags are true, the direct WriteProcessMemory component stamp is skipped and
the published stamp-active flag (`this+0x1338`) becomes false. Repository HEAD
maps this to `m_weaponAttachmentDriven` and IK attachment ownership. The modern
walking changes instead forced the stamp even with a valid rig. This is a
confirmed static difference, not yet a proven runtime/GPU cause of this failure.

Original-DLL evidence:
`ev_255959fa2031ca7f317d2c4d1fe8d04a18ae0a2b86ddc220672f0d5bbe94a688`.
Ghidra output is recovered pseudocode with explicit decompiler warnings, not
source or observed execution. The owner branch itself is present in the output.
Export: `C:/Users/Mastersell/AppData/Local/Temp/opencode/bl1-original-attachment-evidence.json`.
Closed-session snapshot: `bl1-v0573-original-attachment-analysis.json` in that directory.

Candidate `55bb9f3a90cc` restores the original native-owner latch and component/
calibration reset behavior. Canonical placement remains the bootstrap fallback;
once the native IK attachment takes over, competing canonical stamps stop. The
magazine reader now gates on a valid weapon pose, NOT on stamp-enabled status:
native attachment ownership must not disable bone reads. Cached canonical grip/
barrel publication remains available to the stable current-pair IK path. No new
offset, camera write, animation hook, GPU upload hook or controller smoothing.
Ownership transitions log native/canonical translations. A new unconditional
`LT edge` diagnostic precedes reload early exits, even outside the target zone.

Release and 32/32 CTest groups pass, including owner bootstrap/latch/reset and
source contracts allowing only one placement owner. The earlier guard insisting
that valid IK MUST still force canonical writes was an unvalidated design
assumption; it is replaced with the original owner contract. Five settings files
and six camera/arm/timing source hashes are unchanged. These checks do not prove
rendered magazine alignment or retained weapon/hand stability. Native weapon
walking jitter is a validation risk even though early pair preparation and
firing-camera policies remain unchanged.

Candidate was initially PREPARED, NOT INSTALLED while game PID 23576 was running.
Do not terminate it or replace its DLL while running. After closure,
back up current DLL/manifest/log and replace DLL/manifest only. Start the next
headset check with ONE combat rifle: approach the actual magazine and press LT
once even if there is no cue, so the log can distinguish the observed pose from
the input edge. Verify extraction/visibility, then both-eye walking and firing
before asking for another full four-weapon pass.

User subsequently confirmed the game closed. Rechecked game/configurator/
injector absence, the installed `7c47d19124cd` manifest and DLL hash against its
compiled artifact, then installed `55bb9f3a90cc`. Only DLL and manifest were
replaced; both installed hashes verified. Backup under `Binaries/Win64/`:
`vr-backup-20261112-125155-356-native-attachment-55bb9f3a90cc`.
The current INI/profiles/log/commands/loader/configurator and four existing game
settings files were hash-verified unchanged during deployment. No game launched.
Await ONE-rifle reload and retained both-eye/walking/firing stability feedback.

## Successful combined headset validation (2026-11-12)

User reports testing all four weapons: all worked, without jitter or image break.
The installed manifest and log both identify `55bb9f3a90cc`; installed DLL SHA256
`2bf88aae7f66c7fb628b5bcb4f869aba1e2396875c8689a9e380eee2386e5460`
matches its isolated Release artifact. This is now the combined known-good
reference for the reported test, not merely a passing arithmetic test build.

The inspected log additionally observes native IK ownership acquisition and a
complete rifle extraction/pouch/insertion sequence (clip 25 -> 36). Sampled eye
zero/one preparations retain `preparedAtAnimation=1` and matching solved pairs
(1200, 1800, 2400, 3000). `weapon=0` there denotes no direct canonical stamp under
native attachment ownership, NOT a missing weapon pose or failed reload.
The log alone does not establish four complete cycles or visual stability;
that broader confirmation comes from the user's headset report.

At owner transition the logged native and canonical component translations differ
by approximately 40 UU. This supports the competing-placement explanation, but
is not a full native renderer/GPU-upload trace. Retain a single weapon-placement
owner and do not undo early pair preparation or camera protection to fix reload.

Read-only baseline (DLL, manifest, current INI, log snapshot and validation hashes):
`C:/Users/Mastersell/AppData/Local/Temp/opencode/bl1-validated-55bb9f3a90cc-20261112-130450-454`.
Live game files and settings were not modified when recording this success.
No further build was installed. Broader session/runtime/transition coverage and
native ammo reserve accounting remain unvalidated and out of this fix's scope.
