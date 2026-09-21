# Builds sats.wasm, the TRACE module -- the picture half of the door.
# Needs wasi-sdk (34 or later); override with WASI_SDK=... if yours lives elsewhere.
MAKEFLAGS += -r
.SUFFIXES:

WASI_SDK ?= $(HOME)/tools/wasi-sdk-34.0-x86_64-linux
CC        = $(WASI_SDK)/bin/clang

TRACE_INCLUDE = $(firstword $(wildcard ../../../TERMinator-Windows/trace/include) src)

# The build line documented at the top of trace_api.h: the host requires the
# module to IMPORT one shared memory, so -pthread and --import-memory
# --shared-memory are not optional.
CFLAGS  = --target=wasm32-wasip1-threads -O2 -DNDEBUG -std=gnu99 -Wall -Wextra \
          -pthread -I src -I $(TRACE_INCLUDE) -I ../door/third_party
LDFLAGS = -mexec-model=reactor \
          -Wl,--import-memory,--export-memory,--shared-memory,--max-memory=$(MAX_MEMORY) \
          -Wl,--export=wasi_thread_start -Wl,--export-dynamic -Wl,--strip-all -lm

# 64 MB: the frame is 4.9 MB at 2x, the Earth pictures 2.6 MB, the map cache 3.3 MB.
MAX_MEMORY = 67108864

SRC  = src/satmodule.c
DEPS = $(SRC) src/gfx.h src/protocol.h src/fonts.h src/orbit.h

all: sats.wasm

# Uses trace_mouse_mode, so it needs TERMinator 1.1.3+. The door only offers
# TRACE when the Query reply says mouse=1.
sats.wasm: $(DEPS)
	$(CC) $(CFLAGS) $(SRC) $(LDFLAGS) -o $@
	@ls -l $@




clean:
	rm -f sats.wasm

.PHONY: all clean
