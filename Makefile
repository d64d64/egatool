# EGAtool: `make` builds Linux, `make win` cross-compiles Windows, `make all-targets` both.

#
# Needs raylib 6. Linux finds it with pkg-config, or else in /usr/local.
# Override on the command line if it lives elsewhere:
#   make RAYLIB_CFLAGS=-I/opt/raylib/include RAYLIB_LIBS="-L/opt/raylib/lib -lraylib"
#   make win RAYLIB_WIN=~/raylib-6.0_win64_mingw-w64   (has include/ and lib/)

HAVE_PC := $(shell pkg-config --exists raylib 2>/dev/null && echo yes)
ifeq ($(HAVE_PC),yes)
RAYLIB_CFLAGS ?= $(shell pkg-config --cflags raylib)
RAYLIB_LIBS   ?= $(shell pkg-config --libs --static raylib)
else
RAYLIB_CFLAGS ?= -I/usr/local/include
RAYLIB_LIBS   ?= -L/usr/local/lib -lraylib -lm -lpthread -ldl -lrt -lGL
endif

ifdef RAYLIB_WIN
WIN_RAYLIB_CFLAGS ?= -I$(RAYLIB_WIN)/include
WIN_RAYLIB_LIBS   ?= -L$(RAYLIB_WIN)/lib
else
WIN_RAYLIB_CFLAGS ?= $(RAYLIB_CFLAGS)
WIN_RAYLIB_LIBS   ?= -L/usr/local/lib/raylib-win
endif

CFLAGS_BASE   = -std=c99 -O2 -Isrc -Iexternal
CFLAGS_WARN   = -Wall -Wextra
# Third-party single-file libraries: build quietly, with POSIX APIs visible.
CFLAGS_QUIET  = -w -D_DEFAULT_SOURCE

LINUX_CC     = gcc
WIN_CC       = x86_64-w64-mingw32-gcc
WIN_WINDRES  = x86_64-w64-mingw32-windres
WIN_LIBS     = $(WIN_RAYLIB_LIBS) -lraylib -lopengl32 -lgdi32 -lwinmm \
               -lcomdlg32 -lole32 -static -static-libgcc -static-libstdc++

ENGINE_OBJ = engine.o limits.o filters.o imageio.o recipe.o
GUI_OBJ    = $(ENGINE_OBJ) gui.o clipboard.o thread.o raygui_impl.o tinyfiledialogs.o
CLI_OBJ    = $(ENGINE_OBJ) cli.o
HEADERS    = $(wildcard src/*.h)

L = build/linux
W = build/win

.PHONY: linux win all-targets clean

linux: $(L)/egatool $(L)/egatool-cli
win: $(W)/egatool.exe $(W)/egatool-cli.exe
all-targets: linux win

# --- Linux ---
$(L)/egatool: $(addprefix $(L)/,$(GUI_OBJ))
	$(LINUX_CC) $^ -o $@ $(RAYLIB_LIBS) -lm
$(L)/egatool-cli: $(addprefix $(L)/,$(CLI_OBJ))
	$(LINUX_CC) $^ -o $@ -lm
$(L)/raygui_impl.o: src/raygui_impl.c | $(L)
	$(LINUX_CC) $(CFLAGS_BASE) $(CFLAGS_QUIET) $(RAYLIB_CFLAGS) -c $< -o $@
$(L)/tinyfiledialogs.o: external/tinyfiledialogs.c | $(L)
	$(LINUX_CC) $(CFLAGS_BASE) $(CFLAGS_QUIET) $(RAYLIB_CFLAGS) -c $< -o $@
$(L)/%.o: src/%.c $(HEADERS) | $(L)
	$(LINUX_CC) $(CFLAGS_BASE) $(CFLAGS_WARN) $(RAYLIB_CFLAGS) -c $< -o $@

# --- Windows ---
$(W)/egatool.exe: $(addprefix $(W)/,$(GUI_OBJ)) $(W)/icon.o
	$(WIN_CC) -mwindows $^ -o $@ $(WIN_LIBS)
$(W)/egatool-cli.exe: $(addprefix $(W)/,$(CLI_OBJ))
	$(WIN_CC) $^ -o $@ -static -static-libgcc
$(W)/raygui_impl.o: src/raygui_impl.c | $(W)
	$(WIN_CC) $(CFLAGS_BASE) $(CFLAGS_QUIET) $(WIN_RAYLIB_CFLAGS) -c $< -o $@
$(W)/tinyfiledialogs.o: external/tinyfiledialogs.c | $(W)
	$(WIN_CC) $(CFLAGS_BASE) $(CFLAGS_QUIET) $(WIN_RAYLIB_CFLAGS) -c $< -o $@
$(W)/%.o: src/%.c $(HEADERS) | $(W)
	$(WIN_CC) $(CFLAGS_BASE) $(CFLAGS_WARN) $(WIN_RAYLIB_CFLAGS) -c $< -o $@
$(W)/icon.o: src/egatool.rc src/egatool.ico | $(W)
	$(WIN_WINDRES) src/egatool.rc -O coff -o $@

$(L) $(W):
	mkdir -p $@

clean:
	rm -rf $(L) $(W)
