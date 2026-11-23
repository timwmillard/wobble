cmake_minimum_required(VERSION 3.15)

project(wobble C)
set(CMAKE_C_STANDARD 11)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# macOS draws with Metal; Vulkan is only needed on Linux / Windows
if(APPLE)
    set(RAVEN_BACKEND_VULKAN OFF CACHE BOOL "" FORCE)
endif()

add_subdirectory(deps/Raven)

# Build the embedc tool (standalone) and embed gui.xml into gui_xml.h as a
# null-terminated C string (gui_xml_data / gui_xml_len).
add_subdirectory(tool/embedc)

embedc_generate(
    INPUT  ${CMAKE_SOURCE_DIR}/src/gui.xml
    OUTPUT ${CMAKE_SOURCE_DIR}/src/gui_xml.h
    NAME   gui_xml
    TEXT
)
add_custom_target(generate_gui DEPENDS
    ${CMAKE_SOURCE_DIR}/src/gui_xml.h
)

include_directories(.)
include_directories(deps)

if(NOT MSVC)
    set(WARNINGS -Wall -Wextra -Wpedantic)
endif()

# net (webc): HTTP and WebSocket client on the operating system's TLS, copied
# from the chat project. net_conn.c picks the backend; this only adds what
# each one links against.
add_library(net STATIC
    src/net/net_sys.c
    src/net/net_url.c
    src/net/net_conn.c
    src/net/ws.c
    src/net/ws_proto.c
    src/net/http.c
    src/net/http_proto.c
    src/net/sse.c
)
target_include_directories(net PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_options(net PRIVATE ${WARNINGS})
if(APPLE)
    target_link_libraries(net PUBLIC "-framework Network" "-framework Security")
elseif(WIN32)
    target_link_libraries(net PUBLIC ws2_32 secur32 bcrypt)
else()
    target_link_libraries(net PUBLIC ${CMAKE_DL_LIBS} pthread)
endif()

# The game core and the native transport: everything but the UI, shared by
# the app, the bot and the tests.
add_library(wobble_core STATIC
    src/deps.c
    src/json.c
    src/phx.c
    src/game.c
    src/transport_native.c
)
target_include_directories(wobble_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_options(wobble_core PRIVATE ${WARNINGS})
target_link_libraries(wobble_core PUBLIC net)
if(NOT MSVC)
    target_link_libraries(wobble_core PUBLIC m)
endif()

add_executable(${PROJECT_NAME}
    src/main.c
    src/canvas.c
)
add_dependencies(${PROJECT_NAME} generate_gui)
target_compile_options(${PROJECT_NAME} PRIVATE ${WARNINGS})
if(NOT MSVC)
    # gui_xml.h is one long string literal, over C99's 4095-char minimum.
    target_compile_options(${PROJECT_NAME} PRIVATE -Wno-overlength-strings)
endif()
target_link_libraries(${PROJECT_NAME} raven::raven wobble_core)

# Headless bots that join a room and play; for load and protocol testing.
if(NOT WIN32)
    add_executable(wobble_bot src/bot.c)
    target_compile_options(wobble_bot PRIVATE ${WARNINGS})
    target_link_libraries(wobble_bot wobble_core pthread)
endif()

# Tests: ctest --test-dir build
enable_testing()

add_executable(game_test tests/game_test.c)
target_compile_options(game_test PRIVATE ${WARNINGS})
target_link_libraries(game_test wobble_core)
add_test(NAME game_test COMMAND game_test)
