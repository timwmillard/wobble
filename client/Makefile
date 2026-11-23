
.PHONY: all run test

NPROC ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

all: build
	@cmake --build build --parallel $(NPROC)

build:
	@cmake -B build

run: all
	@./build/wobble

clean:
	@rm -rf build


test: all
	@ctest --test-dir build --output-on-failure
