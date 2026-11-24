
.PHONY: all run test e2e web release dmg windows

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

# macOS: a universal (Apple Silicon + Intel) release build, packaged as
# build-release/Wobble-macOS.dmg. Ad-hoc signed only, not notarized.
dmg:
	@cmake -B build-release -DCMAKE_BUILD_TYPE=Release -DWOBBLE_SERVER=$(RELEASE_SERVER) \
		-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
	@cmake --build build-release --parallel $(NPROC)
	@codesign --force --deep --sign - build-release/wobble.app
	@cd build-release && cpack

# Windows, cross-compiled with MinGW-w64 (brew install mingw-w64), packaged as
# build-windows/Wobble-Windows.zip. Raven needs the Vulkan headers: pass
# VULKAN_HEADERS=<dir with vulkan/vulkan.h>, else Raven downloads them.
windows:
	@cmake -B build-windows -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake \
		-DCMAKE_BUILD_TYPE=Release -DWOBBLE_SERVER=$(RELEASE_SERVER) \
		$(if $(VULKAN_HEADERS),-DRAVEN_VULKAN_INCLUDE_DIR=$(VULKAN_HEADERS))
	@cmake --build build-windows --parallel $(NPROC)
	@cd build-windows && cpack

clean:
	@rm -rf build build-web build-release build-windows


test: all
	@ctest --test-dir build --output-on-failure

# End-to-end: starts ../backend on port 4100 with short timers and has bots
# play a full game. BOTS=n, E2E_PORT=p and E2E_TIMEOUT=s override the defaults.
e2e: all
	@tests/e2e.sh build/wobble_bot
