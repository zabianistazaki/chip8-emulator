# Build:  make          -> ./chip8        (needs SDL2 dev package)
# Test:   make test     -> headless core tests under ASan+UBSan (no SDL needed)
# Debug:  make debug    -> ./chip8-debug  (sanitizers, no optimisation)
CC       ?= cc
CFLAGS   ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null || sdl2-config --cflags 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null || sdl2-config --libs 2>/dev/null)

SRC_CORE := src/chip8.c
SRC_MAIN := src/main.c

.PHONY: all test debug clean
all: chip8

chip8: $(SRC_CORE) $(SRC_MAIN) src/chip8.h
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(SRC_CORE) $(SRC_MAIN) -o $@ $(SDL_LIBS)

debug: $(SRC_CORE) $(SRC_MAIN) src/chip8.h
	$(CC) -std=c11 -O0 -g -Wall -Wextra -fsanitize=address,undefined \
	    $(SDL_CFLAGS) $(SRC_CORE) $(SRC_MAIN) -o chip8-debug $(SDL_LIBS)

test: $(SRC_CORE) tests/test_core.c src/chip8.h
	$(CC) -std=c11 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
	    $(SRC_CORE) tests/test_core.c -o test_core
	./test_core

clean:
	rm -f chip8 chip8-debug test_core
