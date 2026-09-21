# Builds the Satellite Tracker door. Runs on the BBS box: it fetches orbits
# (CelesTrak) and places (Open-Meteo), and draws the ANSI view. Needs gcc and,
# at runtime, curl.
MAKEFLAGS += -r
.SUFFIXES:

# Sibling folders on a dev machine, ./native after `make bundle` for the BBS.
MODSRC = $(firstword $(wildcard ../module/src native/module/src))

CC     ?= gcc
CFLAGS ?= -O2 -std=gnu99 -Wall -Wextra -Wno-unused-function -I $(MODSRC) -I . -I third_party

SRC = main.c sats.c ansi_sats.c json.c door.c sha256.c trace_door.c
OBJ = $(SRC:%.c=obj/%.o) obj/image.o

TARGET = satdoor

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $^ -lm -o $@
	@ls -l $@

obj/%.o: %.c $(MODSRC)/orbit.h $(MODSRC)/protocol.h
	@mkdir -p obj
	$(CC) $(CFLAGS) -c $< -o $@

# The stb library (third_party/) is someone else's code: no warnings.
obj/image.o: image.c
	@mkdir -p obj
	$(CC) -O2 -std=gnu99 -w -I . -c $< -o $@

# Fills in whatever the sysop is missing, without clobbering anything.
install: $(TARGET)
	@test -f sats.wasm || cp ../module/sats.wasm . 2>/dev/null || \
		echo "  !! sats.wasm missing -- run make -C ../module"
	@test -f earth.bin || cp ../data/earth.bin . 2>/dev/null || \
		echo "  !! earth.bin missing -- run tools/make_earth.py"
	@test -f sats.ini || cp sats.ini.example sats.ini
	@mkdir -p saves cache
	@echo "Ready. Run: ./$(TARGET) <dir containing door32.sys>"

# Snapshot the module headers so door/ compiles on the BBS with nothing beside it.
bundle:
	rm -rf native || (sleep 2; rm -rf native)
	mkdir -p native/module/src
	cp ../module/src/orbit.h ../module/src/protocol.h native/module/src/
	@echo "bundled"

clean:
	rm -rf obj $(TARGET) preview

.PHONY: all install bundle clean

# The native preview of the TRACE module (tools/render_preview.c).
preview: ../tools/render_preview.c ../module/src/satmodule.c ../module/src/gfx.h ../module/src/orbit.h obj/sats.o obj/json.o obj/door.o
	$(CC) -O2 -g -Wall -Wno-attributes -Wno-unused-function -Wno-format-truncation -I ../module/src -I . -I third_party \
		../tools/render_preview.c obj/sats.o obj/json.o obj/door.o -lm -o $@
