# thumbtool/Makefile — cross-compile slopthumb.exe (Win64 PE) with mingw-w64.
# Same pattern as editor/Makefile (ImGui + DX11/Win32 backends vendored from the
# flake's $IMGUI_DIR). Run inside `nix develop`. The compositor is CPU-side
# (stb_image / stb_truetype / stb_image_resize) so headless export needs no GPU
# and GUI/CLI renders are byte-identical.

CXX      := $(MINGW_CXX)
IMGUI    ?= $(IMGUI_DIR)
JSON_INC ?= $(NLOHMANN_INC)
STBI     ?= $(STB_INC)

BUILD := ../build
OBJ   := $(BUILD)/obj-thumb
OUT   := $(BUILD)/slopthumb.exe
SRC   := src/main.cpp

IMGUI_CORE    := imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp
IMGUI_BACKEND := imgui_impl_win32.cpp imgui_impl_dx11.cpp
IMGUI_MISC    := imgui_stdlib.cpp
IMGUI_OBJ := $(addprefix $(OBJ)/,$(IMGUI_CORE:.cpp=.o) $(IMGUI_BACKEND:.cpp=.o) $(IMGUI_MISC:.cpp=.o))
MAIN_OBJ  := $(OBJ)/main.o
ALL_OBJ   := $(MAIN_OBJ) $(IMGUI_OBJ)

CXXFLAGS := -std=c++17 -O2 -Wall -Wno-unknown-pragmas \
  -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 \
  -I src -I $(IMGUI) -I $(IMGUI)/backends -I $(IMGUI)/misc/cpp -I $(JSON_INC) -I $(STBI) -I $(STBI)/stb
LDFLAGS  := -static -static-libgcc -static-libstdc++ -mconsole
LIBS     := -ld3d11 -ldxgi -ld3dcompiler -lgdi32 -ldwmapi -limm32 -lole32 -luuid -lwinmm -lcomdlg32 -lshell32

$(OUT): $(ALL_OBJ) | $(BUILD)
	$(CXX) $(ALL_OBJ) -o $(OUT) $(LDFLAGS) $(LIBS)
	@echo "built $(OUT)"

$(MAIN_OBJ): $(SRC) src/engine.h | $(OBJ)
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

# Headless render of the committed demo → build/thumb-demo.png (smoke test).
demo: $(OUT)
	cd .. && ./build/slopthumb.exe examples/thumb-demo/demo.thumb.json --export build/thumb-demo.png --proof build/thumb-demo-168.png

shot: $(OUT)
	cd .. && ./build/slopthumb.exe examples/thumb-demo/demo.thumb.json --shot build/thumb-shot.png --frames 5

clean:
	rm -f $(OUT) $(OBJ)/main.o

.PHONY: clean demo shot
