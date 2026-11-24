# Cross-compiling for 64-bit Windows with MinGW-w64, from macOS or Linux:
#   cmake -B build-windows -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
# (make windows). Needs the MinGW-w64 gcc: brew install mingw-w64, or
# apt install gcc-mingw-w64-x86-64.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MINGW_PREFIX x86_64-w64-mingw32)
set(CMAKE_C_COMPILER ${MINGW_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${MINGW_PREFIX}-g++)
set(CMAKE_RC_COMPILER ${MINGW_PREFIX}-windres)

# Programs come from the host; libraries and headers only from the target.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
