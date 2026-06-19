cmake_minimum_required(VERSION 3.24)

project(AxiomCompress
    VERSION 0.1.0
    DESCRIPTION "Experimental archival compressor targeting 7-Zip-class ratios"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Vendored BLAKE3 (portable build — the SSE/AVX backends are intentionally
# disabled so only blake3_portable.c is needed). Third-party C: keep our strict
# warnings off it.
set(BLAKE3_SOURCES
    src/third_party/blake3/blake3.c
    src/third_party/blake3/blake3_dispatch.c
    src/third_party/blake3/blake3_portable.c)

# Vendored Monocypher (XChaCha20-Poly1305 + Argon2id). Single C file; warnings off.
set(MONOCYPHER_SOURCES
    src/third_party/monocypher/monocypher.c)

add_library(axiom STATIC
    src/archive/container.cpp
    src/codec/block.cpp
    src/codec/fast_lz.cpp
    src/codec/lz77.cpp
    src/codec/lz77_split.cpp
    src/core/archive.cpp
    src/core/checksum.cpp
    src/core/cpu.cpp
    src/core/crypto.cpp
    src/core/file_meta.cpp
    src/entropy/huffman.cpp
    src/entropy/range.cpp
    ${BLAKE3_SOURCES}
    ${MONOCYPHER_SOURCES})

target_include_directories(axiom
    PUBLIC include
    PRIVATE src)

target_compile_features(axiom PUBLIC cxx_std_20)

target_compile_definitions(axiom PRIVATE
    BLAKE3_NO_AVX512 BLAKE3_NO_AVX2 BLAKE3_NO_SSE41 BLAKE3_NO_SSE2)

if(MSVC)
    target_compile_options(axiom PRIVATE /W4 /permissive-)
    set_source_files_properties(${BLAKE3_SOURCES} ${MONOCYPHER_SOURCES} PROPERTIES COMPILE_OPTIONS /w)
else()
    target_compile_options(axiom PRIVATE -Wall -Wextra -Wpedantic)
    set_source_files_properties(${BLAKE3_SOURCES} ${MONOCYPHER_SOURCES} PROPERTIES COMPILE_OPTIONS -w)
endif()

# Windows CSPRNG (BCryptGenRandom) used by core/crypto.
if(WIN32)
    target_link_libraries(axiom PUBLIC bcrypt)
endif()

add_executable(axiomc src/cli/main.cpp)
target_link_libraries(axiomc PRIVATE axiom)

if(WIN32)
    add_executable(axiom_gui WIN32
        src/gui/main.cpp
        src/gui/main_window.cpp
        src/gui/custom_menu.cpp
        src/gui/toolbar_icons.cpp
        src/gui/archive_dialogs.cpp
        src/gui/directory_watcher.cpp
        src/gui/settings_store.cpp
        src/gui/browser_model.cpp
        src/gui/operation_runner.cpp
        src/gui/operation_progress_window.cpp)
    target_link_libraries(axiom_gui PRIVATE axiom Comctl32 Dwmapi Msimg32 Ole32 Shell32 Uxtheme)
endif()

enable_testing()

add_executable(axiom_roundtrip tests/roundtrip_test.cpp)
target_link_libraries(axiom_roundtrip PRIVATE axiom)
add_test(NAME axiom_roundtrip COMMAND axiom_roundtrip)
