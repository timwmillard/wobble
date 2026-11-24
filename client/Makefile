
.PHONY: all run test web release

# The server a release build's bare room codes join.
RELEASE_SERVER ?= wss://wobblegame.fly.dev

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

# Native release build against the production server
release:
	@cmake -B build-release -DCMAKE_BUILD_TYPE=Release -DWOBBLE_SERVER=$(RELEASE_SERVER)
	@cmake --build build-release --parallel $(NPROC)

clean:
	@rm -rf build build-web build-release


test: all
	@ctest --test-dir build --output-on-failure
