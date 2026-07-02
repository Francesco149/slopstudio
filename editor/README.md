# editor/

The slopstudio NLE — a Windows-native PE (C++ · Dear ImGui · D3D11), cross-compiled with
mingw-w64 from the Nix flake and run on the Windows host via WSLInterop (no WSLg perf
tax). This is the compositing layer: timeline, pull-based render graph + param-hash GPU
texture cache, instant live preview, deterministic export/record, and the HTTP control
API. See `../docs/ARCHITECTURE.md`.

Build (from the repo root, inside the dev shell):
```sh
nix develop --command make -C editor      # → ../build/slopstudio.exe
```

Dear ImGui is source-vendored (compiled directly into the PE via `$IMGUI_DIR`, the flake's
`pkgs.imgui.src`), following the proven `OpenSummoners/tools/osr_view` pattern. Effects are
authored in GLSL and transpiled to HLSL at load with glslang + SPIRV-Cross.

Layout (as it lands in Phase 1):
- `src/` — app, timeline UI, render graph, compositor, audio, project I/O, control API
- `shaders/` — built-in compositing + effect shaders (GLSL source)
- `Makefile` — single-target mingw build (keep it simple; full rebuild is cheap)
