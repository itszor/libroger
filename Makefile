# libroger: make -> libroger.a; make cov COV=<dir with cov.h> -> libroger-cov.a
# (coverage hooks for a recorder such as SuperSpider's phase0/cov).
CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wno-unused-result -fno-strict-aliasing
SDL_CFLAGS := $(shell pkg-config --cflags sdl3)
SRCS    := $(wildcard src/*.c)
OBJS    := $(SRCS:src/%.c=build/%.o)
COVOBJS := $(SRCS:src/%.c=build/cov/%.o)

all: libroger.a

libroger.a: $(OBJS)
	ar rcs $@ $^

build/%.o: src/%.c src/internal.h include/roger.h include/roger_cpu.h | build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Iinclude -c -o $@ $<

cov: libroger-cov.a
libroger-cov.a: $(COVOBJS)
	ar rcs $@ $^
build/cov/%.o: src/%.c src/internal.h include/roger.h include/roger_cpu.h | build/cov
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -DRECOMP_COV -I$(COV) -Iinclude -c -o $@ $<

build build/cov:
	mkdir -p $@

clean:
	rm -rf build libroger.a libroger-cov.a

# What a game links with: libroger.a plus
#   $(pkg-config --libs sdl3) -lm -lpthread
.PHONY: all cov clean
