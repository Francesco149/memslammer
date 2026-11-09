# memslammer: every frontend links the core directly, so each one is a single
# standalone binary.
#
#   make              CLI + lite GUI for Linux and Windows, harness tools
#   make cli          build/memslammer, build/memslammer.exe
#   make lite         build/memslammer-lite (GTK 3), build/memslammer-lite.exe (Win32)
#   make gui          gui/target/release/memslammer-gui (Rust + gpui-kit, this OS)
#   make test         core self-test (injects bit flips, checks they're caught)
#   make dist         release archives in dist/
#   make live         live USB ISO in build/live/ (live/README.md; needs sudo)
#   make release V=0.2.0   tag v0.2.0 and push it; CI builds and publishes

VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null | sed 's/^v//' || echo dev)
CC ?= cc
WINCC ?= x86_64-w64-mingw32-gcc
WINDRES ?= x86_64-w64-mingw32-windres
WINFLAGS := -D_WIN32_WINNT=0x0601
CFLAGS ?= -O3 -Wall -Wextra
LDFLAGS ?= -s
CFLAGS += -std=gnu11 -DMS_VERSION=\"$(VERSION)\"
AVX2 := -mavx2
GTK_FLAGS = $(shell pkg-config --cflags --libs gtk+-3.0)
# The Linux CLI links musl statically when musl-gcc is installed (70 KB, MIT
# libc); otherwise static glibc (~1 MB). Releases use musl.
MUSLCC ?= $(shell command -v musl-gcc 2>/dev/null)
STATIC_CC := $(if $(MUSLCC),$(MUSLCC),$(CC))

PAWNIO_MODULES ?= 0.2.11

CORE_H := core/memslammer.h core/internal.h
L := build/linux
M := build/static
W := build/win

all: cli lite tools
cli: build/memslammer build/memslammer.exe
lite: build/memslammer-lite build/memslammer-lite.exe
tools: build/memtimings build/memtimings.exe build/karhu_drv.exe
.PHONY: all cli lite tools gui test dist release live clean

# ---- core: engine.c is the only file built with AVX2 ----
$(L)/core.o: core/memslammer.c $(CORE_H) | $(L)
	$(CC) $(CFLAGS) -c -o $@ $<
$(L)/engine.o: core/engine.c $(CORE_H) | $(L)
	$(CC) $(CFLAGS) $(AVX2) -c -o $@ $<
$(M)/core.o: core/memslammer.c $(CORE_H) | $(M)
	$(STATIC_CC) $(CFLAGS) -c -o $@ $<
$(M)/engine.o: core/engine.c $(CORE_H) | $(M)
	$(STATIC_CC) $(CFLAGS) $(AVX2) -c -o $@ $<
$(W)/core.o: core/memslammer.c $(CORE_H) | $(W)
	$(WINCC) $(CFLAGS) $(WINFLAGS) -c -o $@ $<
$(W)/engine.o: core/engine.c $(CORE_H) | $(W)
	$(WINCC) $(CFLAGS) $(WINFLAGS) $(AVX2) -c -o $@ $<
LCORE := $(L)/core.o $(L)/engine.o
MCORE := $(M)/core.o $(M)/engine.o
WCORE := $(W)/core.o $(W)/engine.o

# ---- CLI ----
# Static so it runs on any distro (live USBs have no compiler, or old glibc).
build/memslammer: cli/main.c $(MCORE) core/memslammer.h
	$(STATIC_CC) $(CFLAGS) -o $@ cli/main.c $(MCORE) $(LDFLAGS) -static -pthread
build/memslammer.exe: cli/main.c $(WCORE) $(W)/cli.res.o core/memslammer.h
	$(WINCC) $(CFLAGS) $(WINFLAGS) -o $@ cli/main.c $(WCORE) $(W)/cli.res.o $(LDFLAGS) -static

# ---- lite GUI ----
build/memslammer-lite: lite/gtk.c lite/common.c lite/lite.h $(LCORE)
	$(CC) $(CFLAGS) -o $@ lite/gtk.c lite/common.c $(LCORE) $(GTK_FLAGS) $(LDFLAGS) -pthread
build/memslammer-lite.exe: lite/win32.c lite/common.c lite/lite.h $(WCORE) $(W)/lite.res.o
	$(WINCC) $(CFLAGS) $(WINFLAGS) -municode -mwindows -o $@ lite/win32.c lite/common.c $(WCORE) $(W)/lite.res.o \
	  $(LDFLAGS) -static -lcomctl32 -lole32 -luuid

# ---- Windows resources: icon, manifest, version info ----
$(W)/version.h: FORCE | $(W)
	@v="$(VERSION)"; n=$$(echo "$$v" | sed -n 's/^\([0-9]*\)\.\([0-9]*\)\.\([0-9]*\).*/\1,\2,\3,0/p'); \
	printf '#define MS_VER_COMMA %s\n#define MS_VERSION_STR "%s"\n' "$${n:-0,0,0,0}" "$$v" > $@.tmp; \
	cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@
$(W)/%.res.o: res/%.rc res/common.rc res/app.manifest res/memslammer.ico $(W)/version.h
	$(WINDRES) -I res -I $(W) $< -O coff -o $@
FORCE:

# ---- Rust + gpui-kit app (builds for the OS it runs on) ----
gui:
	cd gui && MS_VERSION=$(VERSION) cargo build --release --locked

# ---- tests ----
build/selftest: tests/selftest.c $(LCORE) $(CORE_H)
	$(CC) $(CFLAGS) -o $@ tests/selftest.c $(LCORE) -pthread
test: build/selftest
	build/selftest

# ---- harness tools (not shipped) ----
build/memtimings: harness/memtimings.c | build
	$(CC) -O2 -Wall -Wextra -o $@ $< -static
build/memtimings.exe: harness/memtimings.c | build
	$(WINCC) -O2 -Wall -Wextra -Wno-cast-function-type -o $@ $< -static
build/karhu_drv.exe: harness/karhu_drv.c | build
	$(WINCC) -O2 -Wall -Wextra -Wno-cast-function-type -o $@ $< -static

# Signed PawnIO module for AMD SMN access (LGPL, fetched, never committed).
build/AMDFamily17.bin: | build
	curl -fsSL -o build/pawnio_modules.zip \
	  https://github.com/namazso/PawnIO.Modules/releases/download/$(PAWNIO_MODULES)/release_$(subst .,_,$(PAWNIO_MODULES)).zip
	unzip -o -q -j build/pawnio_modules.zip AMDFamily17.bin -d build
	rm build/pawnio_modules.zip

# ---- packaging: a minimal archive (CLI + lite) and a GUI archive per OS ----
dist: cli lite
	scripts/package.sh "$(VERSION)"

# Bootable ISO with the CLI, sshd and auto networking, for testing other
# machines without touching their disks. Not part of a release.
live:
	live/build.sh

release:
	@test -n "$(V)" || { echo "usage: make release V=0.2.0"; exit 1; }
	scripts/release.sh "$(V)"

build $(L) $(M) $(W):
	mkdir -p $@

clean:
	rm -rf build dist
