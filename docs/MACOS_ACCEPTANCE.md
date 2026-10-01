# macOS graphical acceptance

Launch GLFW clients and captures in an authorized macOS host-GUI execution context. The restricted Codex command sandbox cannot access the user's Launch Services XPC; there, GLFW may stall in window creation before the game loop or network handshake. This is an execution-context failure, not a server timeout or a reason to change the address, retry policy, or game code. Do not repeat the restricted launch. Use the host execution approval path once and verify that the client connects.

Process-list, port, `lsregister`, and `mdutil` results from the restricted sandbox are not evidence about host state. Do not use them to justify rebuilding the host Launch Services database, changing Spotlight indexing, or rebooting.

Run one server and one graphical client in separate host terminals:

```sh
build/release/mc_main --server --port 20040 --render-distance 72
build/release/mc_main --player-client --address 127.0.0.1:20040
```

The capture client is also graphical and must use the same host context. It exits after a complete player-centered terrain-coverage sweep and framebuffer readback:

```sh
build/release/mc_main --player-client-capture \
    --address 127.0.0.1:20040 \
    --preset central-spike \
    --image /tmp/central-spike.ppm
```

Use only one capture client at a time. Success requires the capture command to exit with status 0 and produce the requested image; a created process or window alone is not acceptance evidence.
