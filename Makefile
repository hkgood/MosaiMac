# Host build for the qd core.
#
# The point of building on the host is that we get a correctness reference and
# a performance baseline without any hardware in the loop. The same sources
# cross-compile for ESP32-S31 under ESP-IDF; see platform/esp32s31.

CC      ?= gcc
CFLAGS  ?= -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion \
           -Wno-sign-conversion -Iinclude
LDFLAGS ?=

CORE_SRC := src/qd_rop.c src/qd_region.c src/qd_expand.c src/qd_win.c
CORE_OBJ := $(CORE_SRC:.c=.o)

# Header dependencies. Without these, editing a header leaves stale .o files
# in place and the tests keep reporting failures that no longer exist -- or,
# worse, keep passing after a behaviour change. Both directions have bitten.
HEADERS := $(wildcard include/qd/*.h) src/qd_internal.h

.PHONY: all test bench clean asan render replay sim sim-app sim-asan app

all: test bench

test: test/test_qd test/test_window
	@./test/test_qd
	@echo
	@./test/test_window

bench: bench/bench_qd
	@./bench/bench_qd

# Regenerate the screenshot used by the README and the GitHub Page. It is
# drawn by the engine itself, not by a design tool, so the image is evidence
# that the ROP and region code works rather than a picture of a picture.
render: build/render
	@./build/render build/render.ppm
	@python3 tools/ppm2png.py build/render.ppm docs/assets/img/workspace.png
	@echo "docs/assets/img/workspace.png regenerated"

build/render: tools/render.c tools/font5x7.c tools/font5x7.h $(CORE_SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) -Itools -o $@ tools/render.c tools/font5x7.c $(CORE_SRC)

# Drive a real drag through the engine and dump every frame. A still image
# reads as a screenshot of something running; a drag is the thing itself.
replay: build/replay
	@mkdir -p build/frames
	@rm -f build/frames/*.ppm
	@./build/replay
	@python3 tools/frames2gif.py

build/replay: tools/replay.c tools/font5x7.c tools/font5x7.h $(CORE_SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) -Itools -o $@ tools/replay.c tools/font5x7.c $(CORE_SRC)

# --- simulator ----------------------------------------------------------
#
# The simulator runs the same core sources as the firmware, so a window drag
# can be iterated on at 60 fps on a Mac instead of at flash-and-pray speed.

SIM_SRC := sim/sim_core.c tools/font5x7.c $(CORE_SRC)

build/sim_headless: sim/sim_headless.c $(SIM_SRC) $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isim -Itools -o $@ sim/sim_headless.c $(SIM_SRC)

# Every pixel of every frame is compared against an independent full repaint,
# so this is a test target, not a demo.
sim: build/sim_headless
	./build/sim_headless build/sim_frames 60

# Same run, but keeping every frame: this is the README animation.
sim-frames: build/sim_headless
	@rm -rf build/sim_frames
	./build/sim_headless build/sim_frames 73 1
	@python3 tools/frames2gif.py

build/sim_asan: sim/sim_headless.c $(SIM_SRC) $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) -fsanitize=address,undefined -Isim -Itools \
	      -o $@ sim/sim_headless.c $(SIM_SRC)
	ASAN_OPTIONS=detect_leaks=1 ./$@ build/asan_frames 60

sim-asan: build/sim_asan
	@true   # the recipe above already ran and verified

build/sim_cocoa: sim/sim_cocoa.m sim/sim_core.c $(CORE_SRC) $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) -Isim -Itools -framework Cocoa -o $@ \
	      sim/sim_cocoa.m sim/sim_core.c $(CORE_SRC)

sim-app: build/sim_cocoa

app:
	@sim/make_app.sh

test/test_qd: test/test_qd.c $(CORE_OBJ) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ test/test_qd.c $(CORE_OBJ) $(LDFLAGS)

test/test_window: test/test_window.c $(CORE_OBJ) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ test/test_window.c $(CORE_OBJ) $(LDFLAGS)

bench/bench_qd: bench/bench_qd.c $(CORE_OBJ) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ bench/bench_qd.c $(CORE_OBJ) $(LDFLAGS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<

# Sanitised run: the ROP engine does a lot of pointer arithmetic and region
# code allocates, so this is the build that catches the mistakes the plain
# build will not.
asan:
	$(CC) -O1 -g -std=c11 -D_POSIX_C_SOURCE=200809L \
	  -fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude \
	  -o /tmp/test_qd_asan test/test_qd.c $(CORE_SRC)
	@/tmp/test_qd_asan
	$(CC) -O1 -g -std=c11 -D_POSIX_C_SOURCE=200809L \
	  -fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude \
	  -o /tmp/test_window_asan test/test_window.c $(CORE_SRC)
	@/tmp/test_window_asan

clean:
	rm -rf build
	rm -f $(CORE_OBJ) test/test_qd test/test_window bench/bench_qd \
	      /tmp/test_qd_asan /tmp/test_window_asan
