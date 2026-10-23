The launcher is an offline Windows GUI executable containing the reviewed game
runtime. It uses the installed MSVC compiler, Windows resource compiler, and
standard Windows APIs. It does not install software or change system settings.

Run the builder from the existing x64 Visual Studio developer shell after the
game candidate and its runtime DLL closure have been frozen and reviewed:

```text
python script\build_windows_singleplayer.py --runtime <runtime-directory> --assets assets --scenarios scenarios --output <new-output-path>\MinecraftClone.exe --expected-game-sha256 <reviewed-game-hash> --source-identity <candidate-identity> --max-extra-bytes <fresh-measured-headroom>
```

The runtime directory must contain mc_main.exe, its redistributable DLLs, and
exactly 16 compiled SPIR-V files in shaders/. The builder snapshots regular files
with the existing package_snapshot helpers. It also includes assets, scenarios,
the game license and HUD licenses. It never downloads dependencies or copies GPU
drivers. A supplied DLL closure must already have passed Windows execution and
dependency inspection. Only the final MinecraftClone.exe goes to the owner;
the manifest sidecar remains with build evidence.

The deterministic RCDATA format is eight bytes MCSINGLE, little-endian uint32
schema=1, uint32 file count, then sorted records: uint32 UTF-8 path length,
uint64 data length, 32 SHA256 bytes, path bytes, data bytes. No executable script
or external archive extractor is used. Payload bytes remain inside the GUI EXE.
The builder verifies the launcher PE has the x64 GUI subsystem. Temporary bundle,
resource, and object files are removed after the build. The manifest records a
conservative peak build allowance of three payload copies plus 4 MiB. The build
owner must also count the delivered EXE and the desktop user's extracted cache
in the total Codex Windows storage ledger.

At launch, files are hash-checked in the user's Known Folder LocalAppData path:
MinecraftClone\SinglePlayer\<bundle SHA256>. Incomplete extraction is staged and
promoted atomically. A root file lock serializes preparation and log rotation;
a shared lease keeps active cache versions out of pruning. Unused cache folders
with this launcher's ownership marker are removed on subsequent launches.
Redirected paths and unowned folders are preserved. A damaged unused cache is
rebuilt from the embedded bytes. No administrator permission or network request
is needed for extraction.

The child is mc_main.exe with zero arguments, with the cache as working directory
and CREATE_NO_WINDOW. It opens its own graphical window and local server through
the ordinary production entry point. The launcher waits for it to close; a normal
close returns its zero exit status. Only log/input handles are inherited. Output
is saved in MinecraftClone\SinglePlayer\logs\latest.log; the previous launch is
rotated to previous.log. Startup or nonzero child exit failures show an actionable
Windows message dialog. The current game server's bind policy is unchanged.

Portable tests:

```text
python -m unittest discover -s script/windows-singleplayer-launcher -p test_*.py
```

Required Windows proof is separate: actual ShellExecute/double-click of the one
EXE, no console window, normal graphical loading and local join, input/full640
state, ordinary window close, child/server termination, and a second launch that
reuses identical cache files. Cache repair and missing-runtime errors must not
be replaced with an artificial test success. Unit tests do not establish this
runtime evidence.
