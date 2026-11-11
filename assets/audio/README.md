# Recorded material impacts

Stone and dirt use distinct edited field recordings instead of variations of the
same procedural impact. These are candidates pending audible owner acceptance.

Source: TinyWorlds, “Different steps on wood, stone, leaves, gravel and mud”.
https://opengameart.org/content/different-steps-on-wood-stone-leaves-gravel-and-mud
Original archive: https://opengameart.org/sites/default/files/%5Bkdd%5DDifferentSteps_0.zip
License: CC0 1.0, https://creativecommons.org/publicdomain/zero/1.0/
The author identifies the sources as pdsounds.org recordings edited for Minetest.
Stone uses `stone01.ogg`; dirt uses `mud02.ogg`. No Minecraft audio is used.

Conversion: decode to mono, retain up to 0.7 seconds, linearly resample to 24 kHz,
apply 5 ms endpoint fades, normalize peak to 2600 signed-16 units. WAV files are
reviewable assets; `.inc` files contain their signed PCM16 samples verbatim.
ClientAudio linearly upsamples once to the mixer's 48 kHz stereo format. No file
I/O or decoding happens on the gameplay event path. `recorded-materials.json`
records the transformed WAV hashes and source file names.

Recreate each include with Python's `wave` and `array` modules: read the WAV's
frames as little-endian signed 16-bit integers, then write comma-separated decimal
integers. Keep the initial provenance comment. Sample changes require listening
in addition to deterministic format, gain and dispatch checks.

# Recorded creation beds

`rain-bed.wav`: Ylmir, *Rain (loopable)*, CC0 1.0.
https://opengameart.org/content/rain-loopable
Original archive: https://opengameart.org/sites/default/files/Rain%20OGG.zip
Uses `1.ogg`, seconds 4–12; explicitly recorded at the author's window.

`boiling-bed.wav`: TinyWorlds, *boiling water loops*, CC0 1.0.
https://opengameart.org/content/boiling-water-loops
Original: https://opengameart.org/sites/default/files/cooking_without_cover_01_0.ogg
Uses seconds 1–9 of a pot-boiling recording as a designed molten-world analogue.

`ice-bed.wav`: northern87, *Ice cracking_northern87*, CC0 1.0.
https://freesound.org/people/northern87/sounds/369982/
Source: https://cdn.freesound.org/previews/369/369982_1196518-hq.mp3
Uses seconds 10–18 of the public high-quality preview of an Oktava MK-012 recording.

Beds are mono24k PCM16, normalized to the peaks in `recorded-beds.json`.
ClientAudio prebuilds four 2-second segments with 150 ms endpoint fades and advances
by 1.85 seconds so adjacent segments crossfade over the same recorded waveform.
Scheduling follows presentation updates; it is not sample-accurate streaming.
Stage changes release the previous segment within 2 seconds. Actual listening and
native device behavior remain required acceptance checks.

Regenerate beds with `uv run --with soundfile --with numpy python assets/audio/prepare-beds.py --source-dir <downloaded-recordings>` using the source filenames in `recorded-beds.json`.

# Packaged music

`welcome.wav`: first 24 seconds of Écrivain's *Meadow Thoughts*, CC0 1.0.
https://opengameart.org/content/meadow-thoughts
Original: https://opengameart.org/sites/default/files/Meadow%20Thoughts.ogg

`gameplay.wav`: first 60 seconds of *Vaporware*, CC0 1.0.
The Cynic Project / cynicmusic.com / pixelsphere.org
https://opengameart.org/content/calm-piano-1-vaporware
Original: https://opengameart.org/sites/default/files/003_Vaporware_2.mp3

Both excerpts are mono 24 kHz PCM16, with two-second cosine endpoint fades,
normalized to peaks 1800 and 1400 respectively. `music.json` records hashes.
Platform loaders supply these ordinary WAV assets before ClientAudio starts.
Ready plays the welcoming harp excerpt once; Gameplay uses the distinct piano
excerpt with a 15-second quiet interval between repetitions. Music load failures
leave material/creation sounds operational. No listening acceptance is claimed.

## Playtest 11 changes

Wind/current stage uses Thimras, *Park ambiences*, CC0 1.0:
https://opengameart.org/content/park-ambiences
https://opengameart.org/sites/default/files/park_ambience_wind.wav
The source is a field recording in an Adelaide park in winter. A three-second
opening excerpt is converted to mono 24 kHz and repeated into the eight-second
embedded bed. This is a wind recording, not a recording of ocean currents.

Music excerpts now retain their source levels without the previous two-second
baked endpoint fades. ClientAudio blends their final two seconds over their
opening, makes a circular segment bank, and repeats both menu and gameplay music
at the normal overlap cadence. The earlier once-only welcome and 15-second
quiet-interval description is superseded. Material action cues now apply an
attack ramp and low-pass smoothing; footsteps use lower gain and a longer attack.
Map open/close cues use softly enveloped forward/reversed CC0 dirt recordings.
Landing is a separate action kind for platform airborne-to-grounded event wiring.
These are source changes, not audible acceptance evidence.
