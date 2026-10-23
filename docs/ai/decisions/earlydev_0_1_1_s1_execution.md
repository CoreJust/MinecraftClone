# S1 implementation execution

Goal: deliver the existing Terrain-v2 R6 world, solid materials, simpler LODs
and already-authorized curvature, with tests, required performance and owner
playtest passing. The ordered implementation/qualification record is
[the JSON ledger](earlydev_0_1_1_s1_execution.json).

## Identity and deadlines

- Checkout: `/Users/core-just/.codex/worktrees/s7-delivery-recovered`, branch `ai-dev`.
- Current committed HEAD: `6db25e1aa78f30406cb44c440f93df2990981237`; active source is dirty and must be identified by manifests.
- Effort D revision 2; preserve unrelated work.
- Original anchor: 19 October 2026, 21:10:43 Europe/Belgrade.
- Original 36-hour deadline: 21 October, 09:10:43 Europe/Belgrade (07:10:43 UTC).

M1–M3.3 missed objectives; the deadline remains unchanged.

## Current scope and implemented work

The owner defers the research terrain-model update to S2. S1 retains R6, simpler
L1/L2/L3 and curvature. Exact CoreLang 0.1.5 ai-dev is accepted; a stable release
is not a gate. Tests, performance and playtest remain required. Exact authority:
JSON `current_state.scope_amendment`.

Exact dependencies: CoreCpp `5c1bad11346cfad44470f5985f1944929952b9a6`
and CoreProject2026 `23f81441c10a44896b87865a9b8ae7d4b51687db`, inexact overrides off; platform qualification remains separate.

Owner correction requires **40 chunks = 640 blocks** of authoritative full-volume
terrain. Prior 40/72-block evidence preserves an assistant unit error and cannot
qualify this contract. Owner playtest 11 failed flicker, x15 coarse-only terrain,
translation lock and LOD joins. Profiles are implemented; owner retest and S1
qualification remain open.

Protocol 20 carries exact session/epoch/token-bound profiles. Dense 48/cache 80
is separate from actual 640/protected 768. S1-v2 requires 6,561 current XY profiles
× 64 Z = 419,904 equivalents; generic 320 evidence stays geometric.

Android temporary-device phase run reaches full Ready with 6,561 profiles, but
55 phase observations have zero source/root publication. The loading/scene defect
remains open; reaching Ready does not prove visible terrain.

Owner playtests 12 and 13 remain **failed**. Playtest 13 reports distance-dependent
terrain changes, a visible pointer after app switching and an apparent hole.
Canonical height nodes now use R6 `sampleSurfaceHeight`, visual identity
`lod-canonical-r6-height-v2`; actual R6 is unchanged. A 170-block counterexample
falls to 0.1054 blocks; 8,436 assertions/18 tests pass. Whole-cell transition and
neighbor segments pass 20 tests. Native focus rearming passes four checks;
ordinary pointer-pixel proof remains open. Altitude-aware horizon expands from
16,384 to 32,768, sea 384, with fog only in its final 4%; 12 CPU and four positive
real-window checks pass.

No-argument local play now binds an ephemeral loopback port and connects to the
actual bound port; it owns and stops its server. Explicit server/port behavior
remains. Windows' 13 MB GUI EXE embeds native game, 16 shaders and assets, extracting
to LocalAppData cache without an archive or console. Controlled ShellExecute
reaches Ready, but foreground acquisition failed; the guard sent no input.
Graphics/Playing/input/cache reuse and owner acceptance remain open.

Audit repairs are implemented: separate bulk/control channels and byte admission;
neutral idle physics; invalid-listener rejection; stale ACK/capability validation;
bounded generation admission and double-precision CPU climate; retained one-shot
input/focus recovery; text/depth synchronization and submitted scene/HUD receipts;
combined scenario budgets; atomic bundle writes and exact 16-shader validation.
Strict evidence now binds moving rendered camera/body, frozen framebuffer/FOV/HUD
settings and current required source/rendered coverage. Verifier 153 checks pass.
Native/GPU integration passes scoped checks; full native 1177/1179 exposed two
fixture-context failures under correction, not a whole-current pass. Owner13,
platform and strict performance acceptance remain open. Old 7d619 is historical.

Windows raw RTX 5070 Ti, 1280×720 immediate presentation fails: about 79.77 fresh
FPS and 0.042548% time in budget; prompt 5.6166 s exceeds 4 s, full 5.511 s meets
15 s. Nested CPU main/render/visible/network means are 6.575/3.199/2.842/2.174 ms;
they are not additive. Driver waits are small. Storage accounting corrected a
36.87 GB cap breach, including missed ProgramData. An 8.609 GB terminal tree is archived (7,113 stable hashes; 197 attribute exceptions). No remote deletion occurred: automatic review rejected it because direct user
authorization is missing. The cleanup question remains pending. All
Windows writes, builds and launches are stopped; owner playtest 12 has since failed.

## Integration and remaining qualification

Prior abfa source passed full native 1,153/1,153 in 161.52 s (exit 0), with reused
fast validation 815 total/12 skipped. Incoming changes remain unverified. Previous
1,149/1,149 and subsequent focused fairness 1/1 remain historical. Reused Python validation totals
815 tests with 12 skipped. Scheduler 35 checks, Mac d72 packaging and Windows
df4e/review 049 remain distinct candidate receipts. All 18 criteria retain qualification gaps.

Latest strict Mac 02 is **36.746% against 99%: FAIL**, superseding 39.664%.
Prompt/full T0 are 2.732/5.226 s; 300/300 sampled updates have 6,561 profiles.
LAN rendered movement covers 2,138 blocks in 40.9 s with responsive input;
10 Hz near-coverage samples are not uninterrupted coverage proof.

Prior gray-band and pacing diagnostics remain candidate-specific, not acceptance.
Continuous LAN coverage, ordinary appearance, platform performance and owner
acceptance remain open. No S1 Ready claim follows.

## Delivery evidence

Preserve frozen identities and distinguish component, performance, platform and
owner gates. Remote publication remains unauthorized.
