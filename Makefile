# Simple POSIX make build (gcc/clang).  On Windows use CMake or build.bat.
CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c99 -Wall -Wextra -Wpedantic -Iinclude
AR      ?= ar

SRC := src/crypto.c src/document.c src/key.c src/rle.c src/util.c \
       src/vfs_read.c src/vfs_write.c
OBJ := $(SRC:src/%.c=build/%.o)

all: build/libsai.a build/saitool build/test_libsai

build:
	mkdir -p build

build/%.o: src/%.c include/sai.h src/sai_internal.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/libsai.a: $(OBJ)
	$(AR) rcs $@ $^

build/saitool: tools/saitool.c build/libsai.a
	$(CC) $(CFLAGS) $< build/libsai.a -o $@

build/test_libsai: tests/test_main.c build/libsai.a
	$(CC) $(CFLAGS) $< build/libsai.a -o $@

test: build/test_libsai
	./build/test_libsai $(SAMPLES)

clean:
	rm -rf build

.PHONY: all test clean
