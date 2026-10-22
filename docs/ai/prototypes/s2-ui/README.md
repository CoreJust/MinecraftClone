# S2 UI prototype

Open `index.html` directly, or serve this directory on localhost. The numbered review links and fixture disclosure sit outside the game frame. Five focused view modules draw exact Tamsyn8x16 bitmap text, native scene capture and bounded map rasters. No in-frame buttons or widget toolkit is required.

## Four views

**Loading.** The unchanged HUD-free S1 capture backs Preparing to connect, the server address, Connecting, cancellation and Enter/Esc hints. Enter advances the review fixture, not a real connection. No percentage, ETA or requested-world readiness is invented. The owner permits this backdrop until completed S1 provides a representative capture.

**Generation.** The same north-up terrain raster used by the world map develops relief, water and climate, then marks the start. Final Arrival/ready terrain uses exactly the default heights+biomes raster, without a separate projection or palette. The 16-second Relief/Water/Climate/Arrival sequence illustrates geographical formation; it is not the actual generation pipeline or progress measurement. Enter advances a sample phase. Reduced motion shows static phase results; hidden pages stop animation. Missing required atlas data stays an error. The exact `Press any key to start` prompt follows the fixture ready state; a fresh key is consumed. Esc cancels; Enter retries cancelled/error states. Production must retain real S1 progress, final coverage, safe spawn, clocks and readiness predicates without waiting for this animation.

**Game.** Position/climate, movement, session health and editing use aligned text groups. Labels and values in each group share one size; all XYZ coordinates use the same scale, with reflow instead of per-value shrinking. All existing desktop fields remain: FPS, uptime, X/Z/Y, yaw/pitch/roll, view, active/selected speed, acceleration, flight and collision bypass. Chunks, memory and marker-local climate are added. `Selected block` and `B Change block` replace `Grass [B]`. F1 toggles telemetry; F5/5 changes view; +/- selects speed; Ctrl toggles acceleration; F6/6 cycles granted movement flags; G changes mode fixtures; B selects Grass/Stone; M opens the map. Survival cannot gain flight, Creative may quick-break Bedrock, Spectator cannot edit. These are UI fixtures, not enforced gameplay permissions.

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

The map uses nearly the full available height with compact layer legends. Heights+biomes start enabled. Temperature provides thermal color with biomes off and isotherms with biomes on; heights retain relief. Individual/composite views have units and legends; all-off retains geography. Arrows pan, Shift pans faster, +/- zooms, R recenters, M/Esc closes. WASD pan is removed because W toggles wind. Held toggles do not flicker, modified browser shortcuts pass through, and map keys cannot change gameplay state.

## Data and limits

`assets/world-data.js` contains 16 aligned fields from one seed42 native revision6 generation: exact stride-2 samples from256x256 to128x128, with unchanged float/integer bits. Its provenance records source snapshots/hashes, configuration, grid, units and decoded field hashes. This samples mutable S1 CPU-generation source; it is not production-model or gameplay qualification. Native captured scene/font provenance and Tamsyn license remain separate.

The marker is normalized148/256,112/256, native world X37888/Y28672. HUD temperature16.6C comes from that marker. Humidity is unavailable: model moisture is not RH; salinity is absent on land. Elevation is geological km relative to sea0, not actual player Z. Runtime counters and Z84.1 remain disclosed examples. Map North is negative raster/nativeY; heading is clockwise, matching native yaw180-heading. The fixed scene is not a camera rendered at that point.

Native/Android/runtime, multiplayer, performance and owner acceptance remain implementation work. Browser screenshots demonstrate the design and controls only.
