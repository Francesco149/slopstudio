{
  description = "slopstudio — high-performance local-first AI video-generation NLE (MIT)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };

        # ── Editor cross-toolchain ────────────────────────────────────────
        # The editor is a 64-bit Windows PE cross-compiled with mingw-w64 and
        # run natively on the Windows host via WSLInterop (no WSLg perf tax).
        # Same approach the sibling OpenSummoners/tools/osr_view uses for its
        # Dear ImGui + DX11 viewer — proven to work here.
        mingw = pkgs.pkgsCross.mingwW64.buildPackages;

        # ── XP capture-harness cross-toolchain (i686 / Windows 5.1) ────────
        # The XP capture harness (tools/xp/) stages tiny agent-less helpers on a
        # QEMU Windows-XP guest — iexec (console-session GUI launcher) + winrect.
        # XP needs a 32-bit PE stamped subsystem 5.1, so a SECOND, narrower cross:
        # i686-w64-mingw32-* (distinct binary prefix from mingwW64 → no PATH clash).
        mingw32 = pkgs.pkgsCross.mingw32.buildPackages;

        # ── FFmpeg/libav, cross-compiled for the Windows PE (in-editor video decode) ──
        # The editor decodes B-roll IN-PROCESS (avformat seek + avcodec decode + swscale
        # → RGBA texture), behind the same clip+time→SRV contract the JPEG proxy used —
        # so dropping an mp4 on the timeline "just works", no proxy step. We build a
        # MINIMAL static ffmpeg: only libav{codec,format,util,swscale}+swresample with
        # FFmpeg's BUILT-IN decoders (h264/hevc/vp9/… need no external libs), every
        # external media dep OFF. The upstream cross ffmpeg is flagged broken only because
        # its default feature set pulls libvmaf (no Windows platform); the stripped build
        # cross-compiles cleanly, so we clear the (now-inapplicable) broken flag.
        ffmpegCross = (pkgs.pkgsCross.mingwW64.ffmpeg.override {
          withHeadlessDeps = false; withSmallDeps = false; withFullDeps = false;  # no external libs
          buildAvcodec = true; buildAvformat = true; buildAvutil = true;
          buildSwscale = true; buildSwresample = true;
          buildAvdevice = false; buildAvfilter = false; buildPostproc = false;
          buildFfmpeg = false; buildFfprobe = false; buildFfplay = false;  # libs only, no CLIs
          withStatic = true; withShared = false; withNetwork = false;       # static → no DLLs to ship
        }).overrideAttrs (old: { meta = old.meta // { broken = false; }; });

        # Dear ImGui is source-vendored (compiled directly into the PE), not a
        # library — we only need the checkout. IMGUI_DIR is exported below.
        imguiSrc = pkgs.imgui.src;

        # Lua 5.4 is source-vendored into the editor PE the same way (its C core is
        # cross-compiled with mingw) for the layout engine's `scene` clip
        # (docs/LAYOUT_ENGINE.md — Lua builds a table tree, Clay lays it out). We expose
        # the UNPACKED source dir; editor/Makefile compiles src/*.c minus the CLI mains.
        luaSrc = pkgs.runCommand "lua-5.4-src" { } ''
          mkdir -p $out
          tar xzf ${pkgs.lua5_4.src} --strip-components=1 -C $out
        '';

        # ── Provider Python env ───────────────────────────────────────────
        # Providers are small out-of-process HTTP/WS services wrapping each
        # model (docs/PROVIDER_PROTOCOL.md). Heavy ML deps (torch, comfyui,
        # qwen-tts, …) are installed on the GPU host, not baked here; this env
        # is for authoring/running the service layer + light media tooling.
        pythonEnv = pkgs.python3.withPackages (ps: with ps; [
          fastapi
          uvicorn
          httpx
          websockets
          pydantic
          pydantic-settings
          aiofiles
          python-multipart
          pillow
          numpy
          soundfile
          requests
          rich
          pytest
          pytest-xdist
        ]);

        # Browser capture is deliberately isolated from the editor shell: it pulls
        # a full Chromium closure, but gives every project one pinned, reproducible
        # way to make high-DPI document sources with provenance sidecars.
        webcapturePython = pkgs.python3.withPackages (ps: [ ps.playwright ]);
      in {
        devShells.default = pkgs.mkShell {
          name = "slopstudio-dev";

          packages = with pkgs; [
            # ── editor build toolchain (Windows PE target) ────────────────
            mingw.gcc            # x86_64-w64-mingw32-{gcc,g++} → Win64 PE
            mingw.binutils
            gnumake
            cmake
            ninja
            pkg-config

            # ── shaders: author effects in GLSL, transpile → HLSL/DXBC ────
            glslang              # GLSL → SPIR-V
            spirv-cross          # SPIR-V → HLSL (for the D3D11 backend)

            # ── editor header-only libs (arch-independent includes) ───────
            nlohmann_json        # project-file JSON parsing
            stb                  # stb_image / stb_image_write (sprites + screenshots)
            lua5_4               # host lua: `slop.py scene-check` runs scene(t,data) headlessly

            # ── media / providers ─────────────────────────────────────────
            ffmpeg               # decode/encode/mux + frame tooling (host side)
            imagemagick          # sprite-sheet slicing, asset prep
            yt-dlp               # reference-video study pipeline (tools/study.py)
            pythonEnv

            # ── general dev ───────────────────────────────────────────────
            git
            git-lfs
            jq
            ripgrep
            fd
            bat
            tree
          ];

          shellHook = ''
            export SLOPSTUDIO_ROOT=$PWD

            # Dear ImGui source checkout for the editor's native build.
            export IMGUI_DIR=${imguiSrc}

            # Lua 5.4 source (layout-engine `scene` clip); editor/Makefile cross-compiles it.
            export LUA_SRC=${luaSrc}

            # Header-only editor libs (used by editor/Makefile).
            export NLOHMANN_INC=${pkgs.nlohmann_json}/include
            export STB_INC=${pkgs.stb}/include

            # FFmpeg/libav cross libs for the editor's in-process video decoder.
            export FFMPEG_CROSS_INC=${ffmpegCross.dev}/include
            export FFMPEG_CROSS_LIB=${ffmpegCross.lib}/lib

            # mingw cross-compiler convenience handles (used by editor/Makefile).
            export MINGW_CC=x86_64-w64-mingw32-gcc
            export MINGW_CXX=x86_64-w64-mingw32-g++
            export MINGW_AR=x86_64-w64-mingw32-ar
            export MINGW_STRIP=x86_64-w64-mingw32-strip

            # The Windows host (for WSLInterop launches + provider reachability).
            export SLOPSTUDIO_WIN_HOST="''${SLOPSTUDIO_WIN_HOST:-cutestation.soy}"

            echo "slopstudio dev shell ready"
            echo "  imgui:    $IMGUI_DIR"
            echo "  lua:      $LUA_SRC (layout engine)"
            echo "  mingw cc: $(command -v $MINGW_CXX || echo '(missing)')"
            echo "  glslang:  $(command -v glslangValidator || echo '(missing)')"
            echo "  libav:    $FFMPEG_CROSS_LIB (in-editor video decode)"
            echo "  python:   $(command -v python) (fastapi/uvicorn/pillow/soundfile)"
            echo ""
            echo "  config:   cp config.example.toml config.toml   # then edit (gitignored)"
            echo "  editor:   nix develop --command make -C editor"
          '';
        };

        # ── XP capture harness shell (tools/xp/) ──────────────────────────
        # Isolated from the lean editor shell so qemu's closure doesn't tax
        # every `nix develop`.  Carries qemu (the VM + screendump/input via QMP),
        # the i686 cross (to build the staged guest helpers iexec/winrect), and
        # the python env (pillow for frame handling).  The SMB control layer
        # (tools/xp/xpsmb.py) shells out to `nix run nixpkgs#netexec` /
        # `nix shell nixpkgs#samba` on demand, so they aren't baked in here.
        #   nix develop .#xp --command python tools/xp/xpvm.py boot --disk …
        devShells.xp = pkgs.mkShell {
          name = "slopstudio-xp";
          packages = with pkgs; [
            pythonEnv
            qemu               # qemu-system-x86_64 + qemu-img (the VM + capture)
            mtools             # build the unattended-install floppy (winnt.sif) — no root
            mingw32.gcc        # i686-w64-mingw32-gcc → XP-subsystem guest helpers
            mingw32.binutils
            gnumake
            jq
          ];
          shellHook = ''
            export SLOPSTUDIO_ROOT=$PWD
            export MINGW32_CC=i686-w64-mingw32-gcc
            echo "slopstudio XP-harness shell"
            echo "  qemu:    $(command -v qemu-system-x86_64 || echo '(missing)')"
            echo "  i686 cc: $(command -v $MINGW32_CC || echo '(missing)')"
            echo "  kvm:     $([ -w /dev/kvm ] && echo 'available' || echo 'NOT writable → TCG (slow)')"
            echo "  build:   make -C tools/xp/win           # iexec.exe + winrect.exe"
            echo "  boot:    python tools/xp/xpvm.py boot --disk cache/xp/xp.qcow2 --fresh"
          '';
        };

        # High-DPI webpage/document capture (tools/capture-web.py).
        #   nix develop .#webcapture --command python tools/capture-web.py URL out.png
        devShells.webcapture = pkgs.mkShell {
          name = "slopstudio-webcapture";
          packages = [ webcapturePython pkgs.playwright-driver.browsers ];
          shellHook = ''
            export PLAYWRIGHT_BROWSERS_PATH=${pkgs.playwright-driver.browsers}
            echo "slopstudio web-capture shell (Chromium + Playwright)"
          '';
        };

        # Codex/ChatGPT desktop browser-control server.  Keeping the app in this
        # locked flake makes .codex/config.toml reproducible instead of relying on
        # an ambient npx install or a versionless global package.
        apps.playwright-mcp = {
          type = "app";
          program = "${pkgs.playwright-mcp}/bin/playwright-mcp";
        };

        formatter = pkgs.nixfmt-rfc-style;
      });
}
