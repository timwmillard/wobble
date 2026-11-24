
.PHONY: all run test web

NPROC ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

all: build
	@cmake --build build --parallel $(NPROC)

build:
	@cmake -B build

run: all
	@if [ -d build/wobble.app ]; then ./build/wobble.app/Contents/MacOS/wobble; else ./build/wobble; fi

# The browser build (needs Emscripten); copies into ../backend/priv/static/play
web: build-web
	@cmake --build build-web --parallel $(NPROC)

build-web:
	@emcmake cmake -B build-web -DCMAKE_BUILD_TYPE=Release

clean:
	@rm -rf build build-web


test: all
	@ctest --test-dir build --output-on-failure
