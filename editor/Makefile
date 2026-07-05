# editor/Makefile — cross-compile slopstudio.exe (Win64 PE) with mingw-w64.
# Dear ImGui + the DX11/Win32 backends are source-vendored from the flake's
# $IMGUI_DIR and compiled directly into the PE (the OpenSummoners/osr_view pattern).
# Run inside `nix develop` so $MINGW_CXX / $IMGUI_DIR / $NLOHMANN_INC / $STB_INC are set.
# NOTE: `:=` (not `?=`) for CXX — the dev shell exports CXX=host-g++, which we must
# override with the mingw cross-compiler (a command-line `make CXX=...` still wins).

CXX      := $(MINGW_CXX)
IMGUI    ?= $(IMGUI_DIR)
JSON_INC ?= $(NLOHMANN_INC)
STBI     ?= $(STB_INC)

# FFmpeg/libav cross libs (from the flake) for the in-process video decoder. When present
# we compile the decoder in (-DSLOP_LIBAV) and link the static libs + the Win32 libs they
# pull (mfuuid/strmiids/bcrypt/atomic, per their pkg-config Libs); absent → proxy-only build.
FFMPEG_INC ?= $(FFMPEG_CROSS_INC)
FFMPEG_LIB ?= $(FFMPEG_CROSS_LIB)
ifneq ($(FFMPEG_LIB),)
  VIDEO_CXXFLAGS := -DSLOP_LIBAV -I $(FFMPEG_INC)
  # Static link order matters: high-level → low-level av libs, THEN the Win32 libs they pull
  # (must come after the archives that reference them). libavcodec's built-in Windows hwaccel
  # objects (dxva2/MediaFoundation) need the COM/MF import libs even though we decode in software.
  VIDEO_LIBS     := -L$(FFMPEG_LIB) -lavformat -lavcodec -lswscale -lswresample -lavutil \
                    -lmfplat -lmfuuid -lstrmiids -lole32 -loleaut32 -lbcrypt -lsecur32 -luser32 -latomic
endif

BUILD := ../build
OBJ   := $(BUILD)/obj
OUT   := $(BUILD)/slopstudio.exe
SRC   := src/main.cpp

# ImGui core + backends compile ONCE into $(OBJ); only main.o rebuilds on edits.
IMGUI_CORE    := imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp
IMGUI_BACKEND := imgui_impl_win32.cpp imgui_impl_dx11.cpp
IMGUI_MISC    := imgui_stdlib.cpp
IMGUI_OBJ := $(addprefix $(OBJ)/,$(IMGUI_CORE:.cpp=.o) $(IMGUI_BACKEND:.cpp=.o) $(IMGUI_MISC:.cpp=.o))
MAIN_OBJ  := $(OBJ)/main.o
ALL_OBJ   := $(MAIN_OBJ) $(IMGUI_OBJ)

CXXFLAGS := -std=c++17 -O2 -Wall -Wno-unknown-pragmas \
  -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 $(VIDEO_CXXFLAGS) \
  -I src -I $(IMGUI) -I $(IMGUI)/backends -I $(IMGUI)/misc/cpp -I $(JSON_INC) -I $(STBI) -I $(STBI)/stb
LDFLAGS  := -static -static-libgcc -static-libstdc++ -mconsole
LIBS     := -ld3d11 -ldxgi -ld3dcompiler -lgdi32 -ldwmapi -limm32 -lole32 -luuid -lwinmm -lwinhttp -lcomdlg32 -lshell32 $(VIDEO_LIBS)

$(OUT): $(ALL_OBJ) | $(BUILD)
	$(CXX) $(ALL_OBJ) -o $(OUT) $(LDFLAGS) $(LIBS)
	@echo "built $(OUT)"

$(MAIN_OBJ): $(SRC) | $(OBJ)
	$(CXX) $(CXXFLAGS) -c $(SRC) -o $@

$(OBJ)/%.o: $(IMGUI)/%.cpp | $(OBJ)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(OBJ)/%.o: $(IMGUI)/backends/%.cpp | $(OBJ)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(OBJ)/%.o: $(IMGUI)/misc/cpp/%.cpp | $(OBJ)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)
$(OBJ):
	mkdir -p $(OBJ)

# Render a headless screenshot of the editor UI and drop it in the build dir.
shot: $(OUT)
	cd .. && ./build/slopstudio.exe examples/signature-opener.slop.json --shot build/shot.png --frames 5

run: $(OUT)
	cd .. && ./build/slopstudio.exe examples/signature-opener.slop.json

clean:
	rm -f $(OUT)

.PHONY: clean run shot
