cmake_minimum_required(VERSION 3.24)

project(AxiomCompress
    VERSION 0.1.0
    DESCRIPTION "Experimental archival compressor targeting 7-Zip-class ratios"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Vendored BLAKE3. x86 SIMD backends are runtime-dispatched by CPUID; other
# architectures use the portable implementation because this source snapshot
# does not include BLAKE3's optional NEON translation unit.
# Third-party C: keep our strict warnings off it.
set(BLAKE3_SOURCES
    src/third_party/blake3/blake3.c
    src/third_party/blake3/blake3_dispatch.c
    src/third_party/blake3/blake3_portable.c)

set(AXIOM_X86 FALSE)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i[3-6]86)$")
    set(AXIOM_X86 TRUE)
    list(APPEND BLAKE3_SOURCES
        src/third_party/blake3/blake3_sse2.c
        src/third_party/blake3/blake3_sse41.c
        src/third_party/blake3/blake3_avx2.c
        src/third_party/blake3/blake3_avx512.c)
endif()

# Vendored Monocypher (XChaCha20-Poly1305 + Argon2id). Single C file; warnings off.
set(MONOCYPHER_SOURCES
    src/third_party/monocypher/monocypher.c)

# Vendored miniz for ZIP container view/extract/test support. Small C library;
# warnings off like the other third-party sources.
set(MINIZ_SOURCES
    src/third_party/miniz/miniz.c
    src/third_party/miniz/miniz_tdef.c
    src/third_party/miniz/miniz_tinfl.c
    src/third_party/miniz/miniz_zip.c)

# minizip-ng's raw container/split-stream core. Axiom uses this narrow backend
# to preserve completed ZIP entries while producing or joining standard split
# sets; normal ZIP Deflate remains on the established miniz provider path.
set(MINIZIP_NG_SOURCES
    src/third_party/minizip-ng/mz_crypt.c
    src/third_party/minizip-ng/mz_os.c
    src/third_party/minizip-ng/mz_strm.c
    src/third_party/minizip-ng/mz_strm_buf.c
    src/third_party/minizip-ng/mz_strm_mem.c
    src/third_party/minizip-ng/mz_strm_split.c
    src/third_party/minizip-ng/mz_zip.c
    src/third_party/minizip-ng/mz_zip_rw.c)
if(WIN32)
    list(APPEND MINIZIP_NG_SOURCES
        src/third_party/minizip-ng/mz_os_win32.c
        src/third_party/minizip-ng/mz_strm_os_win32.c)
else()
    list(APPEND MINIZIP_NG_SOURCES
        src/third_party/minizip-ng/mz_os_posix.c
        src/third_party/minizip-ng/mz_strm_os_posix.c)
endif()

add_library(axiom STATIC
    src/archive/container.cpp
    src/archive/container_formats.cpp
    src/archive/container_zip.cpp
    src/archive/zip_split_backend.cpp
    src/archive/system_provider.cpp
    src/codec/block.cpp
    src/codec/fast_lz.cpp
    src/codec/lz77.cpp
    src/codec/lz77_split.cpp
    src/codec/transform.cpp
    src/core/archive.cpp
    src/core/benchmark_corpus.cpp
    src/core/checksum.cpp
    src/core/checksum_clmul.cpp
    src/core/compression_estimator.cpp
    src/core/cpu.cpp
    src/core/crypto.cpp
    src/core/file_meta.cpp
    src/core/reed_solomon.cpp
    src/entropy/huffman.cpp
    src/entropy/range.cpp
    ${BLAKE3_SOURCES}
    ${MONOCYPHER_SOURCES}
    ${MINIZ_SOURCES}
    ${MINIZIP_NG_SOURCES})

set_source_files_properties(${MINIZIP_NG_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS "MZ_ZIP_NO_CRYPTO;_CRT_SECURE_NO_DEPRECATE;_CRT_NONSTDC_NO_DEPRECATE")

target_include_directories(axiom
    PUBLIC include
    PRIVATE src)

target_compile_features(axiom PUBLIC cxx_std_20)

if(NOT AXIOM_X86)
    target_compile_definitions(axiom PRIVATE
        BLAKE3_NO_SSE2 BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512
        BLAKE3_USE_NEON=0)
endif()

if(MSVC)
    target_compile_options(axiom PRIVATE /W4 /permissive-)
    set_source_files_properties(${BLAKE3_SOURCES} ${MONOCYPHER_SOURCES} ${MINIZ_SOURCES} ${MINIZIP_NG_SOURCES} PROPERTIES COMPILE_OPTIONS /w)
    if(AXIOM_X86)
        set_source_files_properties(src/third_party/blake3/blake3_avx2.c PROPERTIES COMPILE_OPTIONS "/w;/arch:AVX2")
        set_source_files_properties(src/third_party/blake3/blake3_avx512.c PROPERTIES COMPILE_OPTIONS "/w;/arch:AVX512")
    endif()
else()
    target_compile_options(axiom PRIVATE -Wall -Wextra -Wpedantic)
    set_source_files_properties(${BLAKE3_SOURCES} ${MONOCYPHER_SOURCES} ${MINIZ_SOURCES} ${MINIZIP_NG_SOURCES} PROPERTIES COMPILE_OPTIONS -w)
    if(AXIOM_X86)
        set_source_files_properties(src/core/checksum_clmul.cpp PROPERTIES COMPILE_OPTIONS "-msse4.1;-mpclmul")
        set_source_files_properties(src/third_party/blake3/blake3_sse2.c PROPERTIES COMPILE_OPTIONS "-w;-msse2")
        set_source_files_properties(src/third_party/blake3/blake3_sse41.c PROPERTIES COMPILE_OPTIONS "-w;-msse4.1")
        set_source_files_properties(src/third_party/blake3/blake3_avx2.c PROPERTIES COMPILE_OPTIONS "-w;-mavx2")
        set_source_files_properties(src/third_party/blake3/blake3_avx512.c PROPERTIES COMPILE_OPTIONS "-w;-mavx512f;-mavx512vl")
    endif()
endif()

# Windows CSPRNG (BCryptGenRandom) used by core/crypto.
if(WIN32)
    target_link_libraries(axiom PUBLIC bcrypt)
endif()

set(AXIOMC_SOURCES
    src/cli/main.cpp)
if(WIN32)
    list(APPEND AXIOMC_SOURCES src/cli/axiom_cli.rc)
endif()

add_executable(axiomc ${AXIOMC_SOURCES})
target_link_libraries(axiomc PRIVATE axiom)
target_include_directories(axiomc PRIVATE src)

if(WIN32)
    add_executable(axiom_gui WIN32
        src/gui/main.cpp
        src/gui/main_window.cpp
        src/gui/main_window_address.cpp
        src/gui/main_window_browser.cpp
        src/gui/main_window_commands.cpp
        src/gui/main_window_fileops.cpp
        src/gui/main_window_quickadd.cpp
        src/gui/main_window_search.cpp
        src/gui/main_window_theme.cpp
        src/gui/main_window_util.cpp
        src/gui/main_window_views.cpp
        src/gui/custom_menu.cpp
        src/gui/compression_estimate_dialog.cpp
        src/gui/toolbar_icons.cpp
        src/gui/archive_dialogs.cpp
        src/gui/archive_feature_dialogs.cpp
        src/gui/directory_watcher.cpp
        src/gui/drag_drop.cpp
        src/gui/settings_store.cpp
        src/gui/browser_model.cpp
        src/gui/benchmark_dialog.cpp
        src/gui/operation_runner.cpp
        src/gui/operation_progress_window.cpp
        src/gui/sfx_dialog.cpp
        src/gui/dialog_support.cpp
        src/gui/message_dialog.cpp
        src/gui/about_dialog.cpp
        src/gui/update_checker.cpp
        src/gui/axiom_gui.rc)
    target_link_libraries(axiom_gui PRIVATE axiom Bcrypt Comctl32 Dwmapi Gdiplus Msimg32 Ole32 Shell32 Shlwapi Uxtheme Version Winhttp)
    target_include_directories(axiom_gui PRIVATE src)
    target_compile_definitions(axiom_gui PRIVATE UNICODE _UNICODE)
    set_target_properties(axiom_gui PROPERTIES OUTPUT_NAME "Axiom")
endif()

enable_testing()

add_executable(axiom_roundtrip tests/roundtrip_test.cpp)
target_link_libraries(axiom_roundtrip PRIVATE axiom)
target_include_directories(axiom_roundtrip PRIVATE src)
add_test(NAME axiom_roundtrip COMMAND axiom_roundtrip)
