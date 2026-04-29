cmake_minimum_required(VERSION 3.15)
project(mshost)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ─────────────────────────────────────────────
# SOURCES
# ─────────────────────────────────────────────
set(SOURCES
    src/main.cpp

    src/core/application.cpp
    src/core/process_manager.cpp

    src/workers/mc_worker.cpp
    src/workers/web_worker.cpp

    src/services/minecraft_sv_manager.cpp
    src/services/caddy_manager.cpp
    src/services/web_sv.cpp
    src/services/rcon_client.cpp

    src/utils/console.cpp
    src/utils/ipc/ipc_server.cpp
    src/utils/ipc/ipc_client.cpp
)

# ─────────────────────────────────────────────
# TARGET
# ─────────────────────────────────────────────
add_executable(${PROJECT_NAME} ${SOURCES})

# ─────────────────────────────────────────────
# INCLUDE
# ─────────────────────────────────────────────
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ─────────────────────────────────────────────
# DEFINES
# ─────────────────────────────────────────────
target_compile_definitions(${PROJECT_NAME} PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ─────────────────────────────────────────────
# WINDOWS
# ─────────────────────────────────────────────
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ws2_32
        user32
        advapi32
    )
endif()

# ─────────────────────────────────────────────
# OUTPUT
# ─────────────────────────────────────────────
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# ─────────────────────────────────────────────
# WARNINGS
# ─────────────────────────────────────────────
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /utf-8)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
endif()