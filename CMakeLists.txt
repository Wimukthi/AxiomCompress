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

# Bundled AXC codec backends. Zstandard 1.5.7 is built from its official C
# sources with assembly disabled for portable Release/CI builds. The public
# domain LZMA SDK is intentionally built single-threaded inside each chunk;
# Axiom owns the operation-wide worker budget and parallelism.
file(GLOB ZSTD_SOURCES CONFIGURE_DEPENDS
    src/third_party/zstd/lib/common/*.c
    src/third_party/zstd/lib/compress/*.c
    src/third_party/zstd/lib/decompress/*.c)
set(LZMA_SDK_SOURCES
    src/third_party/lzma-sdk/CpuArch.c
    src/third_party/lzma-sdk/LzFind.c
    src/third_party/lzma-sdk/LzFindOpt.c
    src/third_party/lzma-sdk/LzmaEnc.c
    src/third_party/lzma-sdk/LzmaDec.c
    src/third_party/lzma-sdk/Lzma2Enc.c
    src/third_party/lzma-sdk/Lzma2Dec.c)

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
    src/archive/sfx_image.cpp
    src/sfx/sfx_config.cpp
    src/sfx/sfx_options.cpp
    src/archive/seven_zip_library.cpp
    src/archive/zip_split_backend.cpp
    src/archive/system_provider.cpp
    src/codec/block.cpp
    src/codec/external_codecs.cpp
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
    ${ZSTD_SOURCES}
    ${LZMA_SDK_SOURCES}
    ${MINIZIP_NG_SOURCES})

set_source_files_properties(${ZSTD_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS "ZSTD_DISABLE_ASM")
set_source_files_properties(${LZMA_SDK_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS "Z7_ST")
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
    set_source_files_properties(${BLAKE3_SOURCES} ${MONOCYPHER_SOURCES} ${MINIZ_SOURCES} ${ZSTD_SOURCES} ${LZMA_SDK_SOURCES} ${MINIZIP_NG_SOURCES} PROPERTIES COMPILE_OPTIONS /w)
    if(AXIOM_X86)
        set_source_files_properties(src/third_party/blake3/blake3_avx2.c PROPERTIES COMPILE_OPTIONS "/w;/arch:AVX2")
        set_source_files_properties(src/third_party/blake3/blake3_avx512.c PROPERTIES COMPILE_OPTIONS "/w;/arch:AVX512")
    endif()
else()
    target_compile_options(axiom PRIVATE -Wall -Wextra -Wpedantic)
    set_source_files_properties(${BLAKE3_SOURCES} ${MONOCYPHER_SOURCES} ${MINIZ_SOURCES} ${ZSTD_SOURCES} ${LZMA_SDK_SOURCES} ${MINIZIP_NG_SOURCES} PROPERTIES COMPILE_OPTIONS -w)
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
    target_link_libraries(axiom PUBLIC advapi32 bcrypt ole32 oleaut32)
endif()

set(AXIOMC_SOURCES
    src/cli/main.cpp)
if(WIN32)
    list(APPEND AXIOMC_SOURCES
        src/cli/axiom_cli.rc
        src/sfx/module_file.cpp)
endif()

add_executable(axiomc ${AXIOMC_SOURCES})
target_link_libraries(axiomc PRIVATE axiom)
target_include_directories(axiomc PRIVATE src)

if(WIN32)
    set(WIMUKTHI_WIN32_THEME_ROOT
        "${CMAKE_CURRENT_SOURCE_DIR}/../Wimukthi.Win32Theme"
        CACHE PATH "Path to the Wimukthi.Win32Theme source checkout")
    if(NOT EXISTS "${WIMUKTHI_WIN32_THEME_ROOT}/CMakeLists.txt")
        message(FATAL_ERROR
            "Wimukthi.Win32Theme was not found at '${WIMUKTHI_WIN32_THEME_ROOT}'. "
            "Clone it beside AxiomCompress or set WIMUKTHI_WIN32_THEME_ROOT.")
    endif()
    add_subdirectory(
        "${WIMUKTHI_WIN32_THEME_ROOT}"
        "${CMAKE_CURRENT_BINARY_DIR}/wimukthi-win32-theme")

    # The SFX runtimes only need to inspect, verify, and extract AXAR/ZIP
    # payloads. Keep their archive dependency separate from the full provider
    # registry so the Mini image cannot accidentally acquire mutation code.
    file(GLOB SFX_DECODE_ZSTD_SOURCES CONFIGURE_DEPENDS
        src/third_party/zstd/lib/common/*.c
        src/third_party/zstd/lib/decompress/*.c)
    set(SFX_DECODE_LZMA_SOURCES
        src/third_party/lzma-sdk/CpuArch.c
        src/third_party/lzma-sdk/LzmaDec.c
        src/third_party/lzma-sdk/Lzma2Dec.c)
    add_library(axiom_sfx_decode STATIC
        src/sfx/sfx_archive_reader.cpp
        src/archive/container.cpp
        src/archive/container_zip.cpp
        src/archive/sfx_image.cpp
        src/archive/zip_split_backend.cpp
        src/codec/block.cpp
        src/codec/external_codecs_decode.cpp
        src/codec/fast_lz.cpp
        src/codec/lz77.cpp
        src/codec/lz77_split.cpp
        src/codec/transform.cpp
        src/core/archive.cpp
        src/core/checksum.cpp
        src/core/checksum_clmul.cpp
        src/core/cpu.cpp
        src/core/crypto.cpp
        src/core/file_meta.cpp
        src/core/reed_solomon.cpp
        src/entropy/huffman.cpp
        src/entropy/range.cpp
        ${BLAKE3_SOURCES}
        ${MONOCYPHER_SOURCES}
        ${MINIZ_SOURCES}
        ${SFX_DECODE_ZSTD_SOURCES}
        ${SFX_DECODE_LZMA_SOURCES}
        ${MINIZIP_NG_SOURCES})
    target_include_directories(axiom_sfx_decode PUBLIC include PRIVATE src)
    target_compile_features(axiom_sfx_decode PUBLIC cxx_std_20)
    target_compile_definitions(axiom_sfx_decode PRIVATE UNICODE _UNICODE)
    if(NOT AXIOM_X86)
        target_compile_definitions(axiom_sfx_decode PRIVATE
            BLAKE3_NO_SSE2 BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512
            BLAKE3_USE_NEON=0)
    endif()
    set_source_files_properties(${SFX_DECODE_ZSTD_SOURCES} PROPERTIES
        COMPILE_DEFINITIONS "ZSTD_DISABLE_ASM")
    set_source_files_properties(${SFX_DECODE_LZMA_SOURCES} PROPERTIES
        COMPILE_DEFINITIONS "Z7_ST")
    set_source_files_properties(${MINIZIP_NG_SOURCES} PROPERTIES
        COMPILE_DEFINITIONS "MZ_ZIP_NO_CRYPTO;_CRT_SECURE_NO_DEPRECATE;_CRT_NONSTDC_NO_DEPRECATE")
    if(MSVC)
        target_compile_options(axiom_sfx_decode PRIVATE /W4 /permissive-)
        set_source_files_properties(${SFX_DECODE_ZSTD_SOURCES} ${SFX_DECODE_LZMA_SOURCES}
            ${BLAKE3_SOURCES} ${MONOCYPHER_SOURCES} ${MINIZ_SOURCES} ${MINIZIP_NG_SOURCES}
            PROPERTIES COMPILE_OPTIONS /w)
    endif()
    target_link_libraries(axiom_sfx_decode PUBLIC Bcrypt Ole32 OleAut32)

    add_executable(axiom_sfx_module WIN32
        src/sfx/main.cpp
        src/sfx/runtime.cpp
        src/sfx/sfx_config.cpp
        src/sfx/sfx_options.cpp
        src/sfx/sfx_host.cpp
        src/sfx/sfx_dialog_ui.cpp
        src/gui/sfx_dialog.cpp
        src/gui/license_dialog.cpp
        src/gui/archive_feature_dialogs.cpp
        src/gui/operation_progress_window.cpp
        src/gui/dialog_support.cpp
        src/gui/message_dialog.cpp
        src/gui/toolbar_icons.cpp
        src/sfx/sfx_stub.rc)
    target_link_libraries(axiom_sfx_module PRIVATE
        axiom axiom_sfx_decode Wimukthi::Win32Theme Bcrypt Comctl32 Dwmapi Gdiplus
        Msimg32 Ole32 Shell32 Shlwapi Version)
    target_include_directories(axiom_sfx_module PRIVATE src)
    target_compile_definitions(axiom_sfx_module PRIVATE UNICODE _UNICODE)
    set_target_properties(axiom_sfx_module PROPERTIES
        OUTPUT_NAME "AxiomSfx"
        SUFFIX ".bin")

    # Console-only extractor runtime: the same code path without the dialog
    # stack, which is where its much smaller size comes from.
    add_executable(axiom_sfx_mini_module
        src/sfx/mini_main.cpp
        src/sfx/runtime.cpp
        src/sfx/sfx_config.cpp
        src/sfx/sfx_options.cpp
        src/sfx/sfx_host.cpp
        src/sfx/sfx_console_ui.cpp
        src/sfx/sfx_mini_stub.rc)
    target_link_libraries(axiom_sfx_mini_module PRIVATE
        axiom_sfx_decode Bcrypt Ole32 Shell32 Shlwapi)
    target_include_directories(axiom_sfx_mini_module PRIVATE src)
    target_compile_definitions(axiom_sfx_mini_module PRIVATE UNICODE _UNICODE)
    set_target_properties(axiom_sfx_mini_module PROPERTIES
        OUTPUT_NAME "AxiomSfxMini"
        SUFFIX ".bin")

    add_executable(axiom_gui WIN32
        src/sfx/sfx_host.cpp
        src/sfx/sfx_dialog_ui.cpp
        src/gui/license_dialog.cpp
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
        src/sfx/runtime.cpp
        src/sfx/module_file.cpp
        src/gui/axiom_gui.rc)
    target_link_libraries(axiom_gui PRIVATE axiom axiom_sfx_decode Wimukthi::Win32Theme Bcrypt Comctl32 Dwmapi Gdiplus Msimg32 Ole32 Shell32 Shlwapi Version Winhttp)
    target_include_directories(axiom_gui PRIVATE src)
    target_compile_definitions(axiom_gui PRIVATE UNICODE _UNICODE)
    set_target_properties(axiom_gui PROPERTIES OUTPUT_NAME "Axiom")
    add_dependencies(axiom_gui axiom_sfx_module)
    add_dependencies(axiomc axiom_sfx_module)
endif()

enable_testing()

add_executable(axiom_roundtrip tests/roundtrip_test.cpp)
target_link_libraries(axiom_roundtrip PRIVATE axiom)
target_include_directories(axiom_roundtrip PRIVATE src)
add_test(NAME axiom_roundtrip COMMAND axiom_roundtrip)
