# S2 UI — Authentic, informative and engaging

The [four-screen prototype](../prototypes/s2-ui/index.html) is a design artifact,
not game implementation. Judge it against the owner's four criteria: authentic
use of current capabilities, visual appeal, useful information and easy operation.
Prototype animation, field values and ready states are illustrative fixtures.

## Available primitives

Reuse `setGuiText`/`addGuiText`, `TextPlacement` and Tamsyn 8×16 ASCII bitmap
glyphs with position, scale and color. Map/background images supply the visual
surface; hierarchy comes from spacing and typography. Outlined text uses repeated
position-offset glyph draws, already used by PlayerClient. No button toolkit,
panels or new game art is required. The prototype copies the actual font and
native captures. ASCII units use `C`, `%` and `MiB`.

The minimap requires a bounded destination/viewport for today's fullscreen map
draw. Animated generation supplies bounded raster-image updates to that same
primitive. These are explicit S2 integration changes, not existing widgets.
[Adaptive text](earlydev_0_1_1_s2_rendering.md) still needs its scene-sampling
path; do not conceal that work behind the prototype.

## Four screens

1. **Before connection.** A HUD-free pre-rendered game capture backs a clear
   “Preparing to connect” title, supplied destination and current phase. The
   owner permits the current capture as a prototype placeholder; choose a
   representative completed S1 frame for implementation. It is a fixed backdrop,
   not evidence of requested-world readiness. Show “Connecting” only after the
   attempt starts. There is no server-picker/menu or preflight progress API, so
   invent no percentage, ETA or shader checklist. Desktop starts request `T0`
   before connection; Android's `Creating` phase currently falls through to
   “Tectonics 0/0”. Align adapters. Cancel the connection owner and epoch,
   not only its label. Automatic retries retain `T0`; a new request has a new
   epoch and separately retained attempt evidence.

2. **Generation or synchronization.** Make world formation move: relief and
   coast evolution, water/drainage appearance, climate/material transitions,
   then a marked safe spawn. Use bounded map-pixel animation and bitmap phase
   labels. The owner allows illustrative intermediate depiction; this is not a
   claim of physical simulation. Prefer actual stage snapshots and show the
   actual final map before readiness. Distinguish presentation animation from
   measured progress; do not invent completed work, delay readiness for a
   cinematic, or require user input to advance real generation. Join-existing
   uses synchronization/coverage states. Preserve the global map, safe spawn,
   48-block starting-volume predicates and **“Press any key to start”** gate.
   Consume that key; gameplay input follows acknowledgement. Preserve
   [S1 clocks/deadlines](earlydev_0_1_1_s1_loading.md). Reduced motion keeps
   understandable static phase imagery; background animation stops when hidden.

3. **Gameplay.** Keep terrain dominant and information grouped rather than
   removed. Preserve every existing [DebugHud](../../../src/client/render/DebugHud.cpp)
   field and its controls: FPS, uptime/platform touch-flight help, effective and
   selected speed/acceleration, view/F5 or 5, flight/bypass/F6 or 6, XYZ and
   yaw/pitch/roll. Preserve the current coordinate display convention and F1
   visibility. Add dense loaded chunks, process memory and local climate to
   that telemetry group. Keep mode, precise effective edit/movement rights and
   selected block in compact bottom lines, with a central crosshair. Group and
   recolor existing information; no field disappears merely to reduce clutter.
   The local map uses the game's material palette and strong outlined N/E/S/W
   plus heading. The [map note](earlydev_0_1_1_s2_maps.md) owns L1/L2 coverage.

4. **World map.** A dominant image, plain title/detail label, position/heading,
   cardinal glyphs and text hints. Proposed controls: `M`/`Esc` closes, arrows
   pan, `+/-` zoom, `R` recenters. Exclusive map input cannot move/edit/change
   mode while multiplayer continues. Android uses map gestures/back navigation;
   entry mapping and ordinary-device proof remain implementation work.

## Telemetry and visual ownership

Build one immutable UI snapshot outside rendering. Keep completed-frame FPS
semantics, not monitor refresh or CPU-loop counts. Proposed refresh limits are
4 Hz for residency and 1 Hz for memory/climate; no per-frame terrain generation.
“Loaded chunks” means dense resident chunks, separate from full-volume
chunk-equivalents, XY authoritative profiles and visual LOD nodes. Label measured
process-resident memory `Client memory (RSS)`; partial cache/GPU sums are not
RAM totals. Missing measurements use `--`.

Sample climate at normalized player coordinates under matching session/model
identity. The current producer supplies °C and precipitation; atlas moisture
and wind do not define humidity percentage. Humidity remains required scope:
add a documented canonical producer/units, never relabel precipitation/moisture
as relative humidity. Hide salinity where inapplicable; show it elsewhere only
with a meaningful water/salinity source and units. Zero is not “unavailable”.

A local material map must resemble the game: derive colors from canonical
surface materials/shared terrain palette, with bounded relief shading. The
prototype's projection is illustrative, not production LOD evidence. Wider
geographic overview may use atlas colors with an explicit detail label.
Acceptance covers all four flows, dynamic/reduced-motion behavior, every
preserved/new HUD field, conditional data, bitmap contrast, compass visibility,
input gates and narrow landscape. Native/runtime and performance gates remain
future work; a browser screenshot cannot qualify the game.
