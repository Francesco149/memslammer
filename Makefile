CFLAGS ?= -O3 -mavx2 -std=gnu11 -Wall -Wextra
WINCC ?= x86_64-w64-mingw32-gcc

PAWNIO_MODULES ?= 0.2.11

all: build/memslammer build/memslammer.exe build/karhu_drv.exe build/memtimings.exe

build/memslammer: src/memslammer.c | build
	$(CC) $(CFLAGS) -o $@ $< -pthread

build/memslammer.exe: src/memslammer.c | build
	$(WINCC) $(CFLAGS) -o $@ $< -static -pthread

build/karhu_drv.exe: harness/karhu_drv.c | build
	$(WINCC) -O2 -Wall -Wextra -Wno-cast-function-type -o $@ $< -static

build/memtimings.exe: src/memtimings.c | build
	$(WINCC) -O2 -Wall -Wextra -Wno-cast-function-type -o $@ $< -static

# Signed PawnIO module for AMD SMN access (LGPL, fetched, never committed).
build/AMDFamily17.bin: | build
	curl -fsSL -o build/pawnio_modules.zip \
	  https://github.com/namazso/PawnIO.Modules/releases/download/$(PAWNIO_MODULES)/release_$(subst .,_,$(PAWNIO_MODULES)).zip
	unzip -o -q -j build/pawnio_modules.zip AMDFamily17.bin -d build
	rm build/pawnio_modules.zip

build:
	mkdir -p build

clean:
	rm -rf build
.PHONY: all clean
