# S2 UI prototype

Open `index.html` directly, or serve this directory on localhost. The numbered review links and fixture disclosure sit outside the game frame. Five focused view modules draw exact Tamsyn8x16 bitmap text, native scene capture and bounded map rasters. No in-frame buttons or widget toolkit is required.

## Four views

**Loading.** The unchanged HUD-free S1 capture backs Preparing to connect, the server address, Connecting, cancellation and Enter/Esc hints. Enter advances the review fixture, not a real connection. No percentage, ETA or requested-world readiness is invented. The owner permits this backdrop until completed S1 provides a representative capture.

**Generation.** A 30-second illustrative history moves rigid native plate regions, develops collision/rift relief and changing coasts, fills water basins, advances glaciers/sea ice, and settles climate. Six stages are Tectonics/Relief/Water/Ice/Climate/Arrival. Ice grows toward the shared retained final mask. Climate adds sparse native-field wind/current paths and rain; currents stay on water and rain follows wet fields. These are illustrative dynamics, not recorded history or physical simulation. Arrival/ready draw the exact normal heights+biomes atlas without temporary motion/weather. Enter advances a phase; reduced motion shows static stage results; hidden pages stop animation. Errors/cancel/retry remain actionable. `Press any key to start` consumes a fresh key. Production keeps real S1 progress, coverage, spawn, clocks and readiness without waiting for this example animation.

**Game.** Position/climate, movement, session health and editing use aligned text groups. Labels and values in each group share one size; all XYZ coordinates use the same scale, with reflow instead of per-value shrinking. FPS, uptime, X/Z/Y, yaw/pitch/roll, active/selected speed, acceleration, flight and collision bypass remain. The owner removes only the View readout; F5/5 camera behavior/help stays. Restrained group/status colors preserve the same readout size. Chunks, memory and marker-local climate are added. `Selected block` and `B Change block` replace `Grass [B]`. F1 toggles telemetry; F5/5 changes view; +/- selects speed; Ctrl toggles acceleration; F6/6 cycles granted movement flags; G changes mode fixtures; B selects Grass/Stone; M opens the map. Survival cannot gain flight, Creative may quick-break Bedrock, Spectator cannot edit. These are UI fixtures, not enforced gameplay permissions.

The minimap shows a periodic local crop using native relief and material-family colors. Its climate-derived surface colors are explicitly a proxy, not verified voxel surfaces or production L1/L2 coverage. N/E/S/W and clockwise heading are prominent.

**World map.** Press a layer key to toggle it independently:

| Key | Layer |
|---|---|
| P | Plate boundaries and drift |
| H | Geological heights, relief and contours |
| T | Temperature |
| B | Biomes represented by coarse climate zones |
| W | Wind direction |
| C | Ocean-current direction |
| O | Model moisture |
| Q | Model precipitation |

The map uses the full usable viewport with a closer default view; `0` shows the entire world, `R` resets and `L` expands the compact legend. Projection stays isotropic. Heights+biomes start enabled. Temperature, moisture and precipitation are independent translucent continuous heatmaps over geographic context, with numeric/color keys and truthful model units. Individual/composite views and all-off remain useful. Arrows pan, Shift pans faster, +/- zooms, M/Esc closes. W toggles wind, so WASD does not pan. Held toggles do not flicker, browser shortcuts pass through, and map keys cannot change gameplay state.

## Data and limits

`assets/world-data.js` contains 16 aligned fields from one seed42 native revision6 generation: exact stride-2 samples from256x256 to128x128, with unchanged float/integer bits. Its provenance records source snapshots/hashes, configuration, grid, units and decoded field hashes. This samples mutable S1 CPU-generation source; it is not production-model or gameplay qualification. Native captured scene/font provenance and Tamsyn license remain separate.

Shared ice is a visual proxy: 4,303 nonwater Snow-zone cells and 2,834 ocean/lake cells at/below -10C. No native ice producer exists here; this threshold is not a seawater-freezing law. The normal map retains this same mask; native fields remain unchanged.

The marker is normalized148/256,112/256, native world X37888/Y28672. HUD temperature16.6C comes from that marker. Humidity is unavailable: model moisture is not RH; salinity is absent on land. Elevation is geological km relative to sea0, not actual player Z. Runtime counters and Z84.1 remain disclosed examples. Map North is negative raster/nativeY; heading is clockwise, matching native yaw180-heading. The fixed scene is not a camera rendered at that point.

Native/Android/runtime, multiplayer, performance and owner acceptance remain implementation work. Browser screenshots demonstrate the design and controls only.
