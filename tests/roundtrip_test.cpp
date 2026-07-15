#include "axiom/archive.hpp"
#include "axiom/axiom.hpp"
#include "codec/block.hpp"
#include "codec/fast_lz.hpp"
#include "codec/lz77.hpp"
#include "codec/lz77_split.hpp"
#include "codec/transform.hpp"
#include "core/checksum.hpp"
#include "core/cpu.hpp"
#include "core/hash.hpp"
#include "core/file_replace.hpp"
#include "core/reed_solomon.hpp"
#include "core/task_executor.hpp"
#include "entropy/huffman.hpp"
#include "entropy/range.hpp"
#include "third_party/miniz/miniz.h"

#include "check.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

#if defined(_WIN32)
std::optional<std::filesystem::path> bundled_7z_for_tests();
#endif

std::vector<std::uint8_t> bytes_from_string(const std::string& text) {
    return {text.begin(), text.end()};
}

std::string utf8_bytes(std::u8string_view text) {
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

void expect_roundtrip(const std::vector<std::uint8_t>& input) {
    const auto archive = axiom::compress(input);
    const auto restored = axiom::decompress(archive);
    AXIOM_CHECK(restored == input);
}

void expect_huffman_roundtrip(const std::vector<std::uint8_t>& input) {
    const auto encoded = axiom::entropy::encode_huffman(input);
    AXIOM_CHECK(encoded.has_value());

    const auto restored = axiom::entropy::decode_huffman(*encoded, input.size());
    AXIOM_CHECK(restored == input);
}

void expect_order1_roundtrip(const std::vector<std::uint8_t>& input) {
    const auto encoded = axiom::entropy::encode_order1(input);
    AXIOM_CHECK(encoded.has_value());

    const auto restored = axiom::entropy::decode_order1(*encoded, input.size());
    AXIOM_CHECK(restored == input);
}

void expect_rans_roundtrip(const std::vector<std::uint8_t>& input) {
    const auto encoded = axiom::entropy::encode_rans(input);
    AXIOM_CHECK(encoded.has_value());

    const auto restored = axiom::entropy::decode_rans(*encoded, input.size());
    AXIOM_CHECK(restored == input);
}

void expect_rans_order1_roundtrip(const std::vector<std::uint8_t>& input) {
    const auto encoded = axiom::entropy::encode_rans_order1(input);
    AXIOM_CHECK(encoded.has_value());

    const auto restored = axiom::entropy::decode_rans_order1(*encoded, input.size());
    AXIOM_CHECK(restored == input);
}

void expect_parallel_block_roundtrip(const std::vector<std::uint8_t>& input) {
    axiom::CompressionOptions options;
    options.block_size = 1024;
    options.thread_count = 4;

    const auto encoded = axiom::codec::encode_parallel_blocks(input, options);
    const auto restored = axiom::codec::decode_parallel_blocks(encoded, input.size(), options.thread_count);
    AXIOM_CHECK(restored == input);
}

void expect_fast_lz_roundtrip(const std::vector<std::uint8_t>& input) {
    axiom::CompressionOptions options;
    options.use_fast_lz = true;
    options.nice_length = 64;

    const auto encoded = axiom::codec::encode_fast_lz(input, options);
    AXIOM_CHECK(axiom::codec::decode_fast_lz(encoded, input.size()) == input);

    const auto lz77 = axiom::codec::encode_fast_lz77(input, options);
    AXIOM_CHECK(axiom::codec::decode_lz77(lz77, input.size()) == input);

    const auto archive = axiom::compress(input, options);
    AXIOM_CHECK(axiom::decompress(archive) == input);
}

void expect_split_stream_roundtrip(const std::vector<std::uint8_t>& input) {
    const auto lz77 = axiom::codec::encode_lz77(input, {});
    const auto split = axiom::codec::encode_lz77_split_streams(lz77);
    const auto restored = axiom::codec::decode_lz77_split_streams(split, input.size());
    AXIOM_CHECK(restored == input);
}

void expect_slot_split_stream_roundtrip(const std::vector<std::uint8_t>& input) {
    const auto lz77 = axiom::codec::encode_lz77(input, {});
    const auto slots = axiom::codec::encode_lz77_split_streams_slots(lz77);
    AXIOM_CHECK(slots.has_value());

    const auto restored = axiom::codec::decode_lz77_split_streams_slots(*slots, input.size());
    AXIOM_CHECK(restored == input);
}

void expect_nested_task_executor() {
    axiom::core::TaskExecutor executor(8);
    std::atomic_size_t completed = 0;
    auto parent = executor.submit([&] {
        std::vector<std::future<std::size_t>> children;
        children.reserve(256);
        for (std::size_t i = 0; i < 256; ++i) {
            children.push_back(executor.submit([&, i] {
                completed.fetch_add(1, std::memory_order_relaxed);
                return i;
            }));
        }
        std::size_t sum = 0;
        for (auto& child : children) {
            sum += executor.wait(child);
        }
        return sum;
    });

    AXIOM_CHECK(executor.wait(parent) == (255u * 256u) / 2u);
    AXIOM_CHECK(completed.load(std::memory_order_relaxed) == 256);
}

void expect_sequence_stream_roundtrip(const std::vector<std::uint8_t>& input) {
    const auto lz77 = axiom::codec::encode_lz77(input, {});
    const auto sequences =
        axiom::codec::encode_lz77_sequence_streams(input, lz77, /*fast=*/false);
    AXIOM_CHECK(sequences.has_value());
    AXIOM_CHECK(axiom::codec::decode_lz77_sequence_streams(*sequences, input.size()) == input);
    AXIOM_CHECK(!axiom::codec::encode_lz77_sequence_streams(
        input, lz77, /*fast=*/false, /*maximum_useful_size=*/0));

    const auto candidates =
        axiom::codec::encode_lz77_payload_candidates(input, lz77, /*fast=*/false);
    AXIOM_CHECK(candidates.sequence || candidates.split || candidates.slots);
    if (candidates.sequence) {
        AXIOM_CHECK(*candidates.sequence == *sequences);
        AXIOM_CHECK(axiom::codec::decode_lz77_sequence_streams(
                        *candidates.sequence, input.size()) == input);
    }
    if (candidates.split) {
        AXIOM_CHECK(axiom::codec::decode_lz77_split_streams(
                        *candidates.split, input.size()) == input);
    }
    if (candidates.slots) {
        AXIOM_CHECK(axiom::codec::decode_lz77_split_streams_slots(
                        *candidates.slots, input.size()) == input);
    }
}

// The fast entropy chooser must produce archives that decode identically; only
// the encoder's coder selection differs from the full bake-off.
void expect_fast_entropy_roundtrip(const std::vector<std::uint8_t>& input) {
    const auto lz77 = axiom::codec::encode_lz77(input, {});

    const auto split = axiom::codec::encode_lz77_split_streams(lz77, /*fast=*/true);
    AXIOM_CHECK(axiom::codec::decode_lz77_split_streams(split, input.size()) == input);

    if (auto slots = axiom::codec::encode_lz77_split_streams_slots(lz77, /*fast=*/true)) {
        AXIOM_CHECK(axiom::codec::decode_lz77_split_streams_slots(*slots, input.size()) == input);
    }
}

// End-to-end through compress()/decompress() with the fast speed knobs engaged.
void expect_fast_archive_roundtrip(const std::vector<std::uint8_t>& input) {
    axiom::CompressionOptions options;
    options.fast_entropy = true;
    options.lazy_matching = true;
    options.max_chain_depth = 16;
    const auto archive = axiom::compress(input, options);
    AXIOM_CHECK(axiom::decompress(archive) == input);
}

void test_compression_level_presets() {
    axiom::CompressionOptions fast;
    axiom::apply_compression_level(fast, 0);
    AXIOM_CHECK(fast.use_fast_lz);
    AXIOM_CHECK(fast.fast_entropy);
    AXIOM_CHECK(!fast.lazy_matching);
    AXIOM_CHECK(fast.max_chain_depth == 8);

    axiom::CompressionOptions balanced;
    axiom::apply_compression_level(balanced, 5);
    AXIOM_CHECK(!balanced.use_fast_lz);
    AXIOM_CHECK(!balanced.use_tree_matcher);
    AXIOM_CHECK(!balanced.fast_entropy);
    AXIOM_CHECK(balanced.lazy_matching);
    AXIOM_CHECK(balanced.max_chain_depth == 128);

    axiom::CompressionOptions stronger_chain;
    axiom::apply_compression_level(stronger_chain, 6);
    AXIOM_CHECK(!stronger_chain.use_tree_matcher);
    AXIOM_CHECK(stronger_chain.max_chain_depth == 256);
    AXIOM_CHECK(stronger_chain.nice_length == 192);

    axiom::CompressionOptions lazy_tree;
    lazy_tree.lazy_matching = false;
    axiom::apply_compression_level(lazy_tree, 7);
    AXIOM_CHECK(lazy_tree.use_tree_matcher);
    AXIOM_CHECK(lazy_tree.lazy_matching);
    AXIOM_CHECK(!lazy_tree.enable_optimal_parser);

    axiom::CompressionOptions wide_tree;
    axiom::apply_compression_level(wide_tree, 8);
    AXIOM_CHECK(wide_tree.use_tree_matcher);
    AXIOM_CHECK(!wide_tree.lazy_matching);
    AXIOM_CHECK(wide_tree.max_chain_depth == 128);
    AXIOM_CHECK(wide_tree.block_size == (32u << 20));
    AXIOM_CHECK(wide_tree.window_size == (32u << 20));
    wide_tree.thread_count = 16;
    AXIOM_CHECK(axiom::codec::effective_parallel_block_size(64u << 20, wide_tree) ==
                (4u << 20));

    axiom::CompressionOptions maximum;
    axiom::apply_compression_level(maximum, 99);
    AXIOM_CHECK(maximum.use_tree_matcher);
    AXIOM_CHECK(!maximum.use_fast_lz);
    AXIOM_CHECK(maximum.max_chain_depth == 512);
    AXIOM_CHECK(maximum.max_match == 4096);
    AXIOM_CHECK(maximum.block_size == (64u << 20));
    AXIOM_CHECK(maximum.window_size == (64u << 20));
    AXIOM_CHECK(!maximum.optimal_two_pass);
    AXIOM_CHECK(maximum.auto_block_size_for_threads);
}
void expect_tree_lz77_roundtrip(const std::vector<std::uint8_t>& input) {
    axiom::CompressionOptions options;
    options.use_tree_matcher = true;
    const auto lz77 = axiom::codec::encode_lz77(input, options);
    const auto restored = axiom::codec::decode_lz77(lz77, input.size());
    AXIOM_CHECK(restored == input);
}

// Drive the cyclic-window tree matcher with a window much smaller than the input
// so the cyclic buffer wraps many times and node deletion is exercised. Any
// tree-logic bug surfaces as a decode mismatch (matches are byte-validated, so a
// bad match cannot corrupt the stream — only fail to round-trip if reps drift).
void expect_tree_lz77_windowed_roundtrip(const std::vector<std::uint8_t>& input,
                                         std::size_t window_size) {
    axiom::CompressionOptions options;
    options.use_tree_matcher = true;
    options.window_size = window_size;
    const auto lz77 = axiom::codec::encode_lz77(input, options);
    const auto restored = axiom::codec::decode_lz77(lz77, input.size());
    AXIOM_CHECK(restored == input);
}

void expect_lazy_lz77_roundtrip(const std::vector<std::uint8_t>& input) {
    axiom::CompressionOptions options;
    options.lazy_matching = true;
    options.max_chain_depth = 16;  // shallow chain is where lazy matters most
    const auto lz77 = axiom::codec::encode_lz77(input, options);
    const auto restored = axiom::codec::decode_lz77(lz77, input.size());
    AXIOM_CHECK(restored == input);
}

void expect_optimal_lz77_roundtrip(const std::vector<std::uint8_t>& input) {
    axiom::CompressionOptions options;
    options.optimal_chain_depth = 2;
    options.max_parser_candidates = 2;

    const auto lz77 = axiom::codec::encode_lz77_optimal(input, options);
    const auto restored = axiom::codec::decode_lz77(lz77, input.size());
    AXIOM_CHECK(restored == input);
}

// Reference one-byte-per-step reflected CRC-32, independent of the production
// slice-by-8 tables, so a table-generation bug is caught.
std::uint32_t crc32_reference(const std::vector<std::uint8_t>& data) {
    std::uint32_t state = 0xFFFFFFFFu;
    for (const auto byte : data) {
        state ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            state = (state & 1u) ? (0xEDB88320u ^ (state >> 1)) : (state >> 1);
        }
    }
    return state ^ 0xFFFFFFFFu;
}

// The interleaved rANS must round-trip at every length near the lane boundary,
// for a single-symbol stream (degenerate frequency table), and for skewed data.
// Reed-Solomon erasure coding: any up to `parity` erased shards reconstruct exactly,
// and erasing more than that is reported unrecoverable.
void test_reed_solomon() {
    std::mt19937 rng(0x9E3779B9u);
    for (int trial = 0; trial < 150; ++trial) {
        const int d = 1 + static_cast<int>(rng() % 16);
        const int p = 1 + static_cast<int>(rng() % 6);
        const std::size_t len = 1 + (rng() % 128);
        axiom::core::ReedSolomon rs(d, p);
        const int total = d + p;

        std::vector<std::vector<std::uint8_t>> shards(total, std::vector<std::uint8_t>(len));
        for (int i = 0; i < d; ++i) {
            for (auto& b : shards[static_cast<std::size_t>(i)]) {
                b = static_cast<std::uint8_t>(rng() & 0xFF);
            }
        }
        std::vector<std::span<const std::uint8_t>> dv;
        for (int i = 0; i < d; ++i) {
            dv.emplace_back(shards[static_cast<std::size_t>(i)]);
        }
        std::vector<std::span<std::uint8_t>> pv;
        for (int i = 0; i < p; ++i) {
            pv.emplace_back(shards[static_cast<std::size_t>(d + i)]);
        }
        rs.encode(dv, pv);
        const auto original = shards;

        std::vector<int> idx(static_cast<std::size_t>(total));
        for (int i = 0; i < total; ++i) {
            idx[static_cast<std::size_t>(i)] = i;
        }
        std::shuffle(idx.begin(), idx.end(), rng);
        const int erasures = static_cast<int>(rng() % static_cast<unsigned>(p + 1));
        std::vector<bool> present(static_cast<std::size_t>(total), true);
        for (int e = 0; e < erasures; ++e) {
            present[static_cast<std::size_t>(idx[static_cast<std::size_t>(e)])] = false;
            shards[static_cast<std::size_t>(idx[static_cast<std::size_t>(e)])].assign(len, 0);
        }
        AXIOM_CHECK(rs.reconstruct(shards, present));
        AXIOM_CHECK(shards == original);
    }

    // Losing more shards than there is parity (and dropping below `data` survivors)
    // is unrecoverable.
    axiom::core::ReedSolomon rs(6, 2);
    std::vector<std::vector<std::uint8_t>> shards(8, std::vector<std::uint8_t>(10, 1));
    std::vector<bool> present(8, true);
    present[0] = present[1] = present[2] = false;  // 5 survive < 6 data shards
    AXIOM_CHECK(!rs.reconstruct(shards, present));
}

void test_rans_edges() {
    for (std::size_t len = 0; len <= 40; ++len) {
        std::vector<std::uint8_t> single(len, 0xABu);
        if (auto e = axiom::entropy::encode_rans(single)) {
            AXIOM_CHECK(axiom::entropy::decode_rans(*e, len) == single);
        }

        std::mt19937 rng(static_cast<unsigned>(len) * 2654435761u);
        std::vector<std::uint8_t> skewed(len);
        for (auto& byte : skewed) {
            byte = static_cast<std::uint8_t>((rng() % 4 == 0) ? rng() : 65);  // mostly 'A'
        }
        if (auto e = axiom::entropy::encode_rans(skewed)) {
            AXIOM_CHECK(axiom::entropy::decode_rans(*e, len) == skewed);
        }
    }
}

std::string to_hex(std::span<const std::uint8_t> bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (const auto b : bytes) {
        out += digits[b >> 4];
        out += digits[b & 0x0F];
    }
    return out;
}

void test_blake3() {
    // Official BLAKE3 known-answer vectors — confirms the vendored library is
    // really BLAKE3, not just internally self-consistent.
    AXIOM_CHECK(to_hex(axiom::core::Blake3::hash({})) ==
                "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
    const std::vector<std::uint8_t> abc = {'a', 'b', 'c'};
    AXIOM_CHECK(to_hex(axiom::core::Blake3::hash(abc)) ==
                "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85");

    // Incremental hashing must equal one-shot, across the 1 KiB chunk boundary.
    std::vector<std::uint8_t> data(5000);
    std::mt19937 rng(0xB3u);
    for (auto& byte : data) {
        byte = static_cast<std::uint8_t>(rng());
    }
    axiom::core::Blake3 hasher;
    hasher.update(std::span(data).subspan(0, 1500));
    hasher.update(std::span(data).subspan(1500));
    AXIOM_CHECK(hasher.finalize() == axiom::core::Blake3::hash(data));
}

void test_crc32() {
    // Standard check value: CRC-32/ISO-HDLC of "123456789" is 0xCBF43926.
    const std::vector<std::uint8_t> check = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    AXIOM_CHECK(axiom::core::crc32(check) == 0xCBF43926u);

    // The slice-by-8 kernel must match the reference at every length boundary
    // (the 8-byte stride, its tail, and the empty input).
    std::mt19937 rng(0xC0FFEEu);
    std::vector<std::uint8_t> data;
    for (std::size_t len = 0; len <= 600; ++len) {
        data.resize(len);
        for (auto& byte : data) {
            byte = static_cast<std::uint8_t>(rng());
        }
        AXIOM_CHECK(axiom::core::crc32(data) == crc32_reference(data));
    }

    // Incremental updates must equal a single pass over the concatenation.
    data.resize(500);
    for (auto& byte : data) {
        byte = static_cast<std::uint8_t>(rng());
    }
    auto state = axiom::core::crc32_init();
    state = axiom::core::crc32_update(state, std::span(data).subspan(0, 123));
    state = axiom::core::crc32_update(state, std::span(data).subspan(123));
    AXIOM_CHECK(axiom::core::crc32_final(state) == axiom::core::crc32(data));

    const auto left = std::span(data).subspan(0, 177);
    const auto right = std::span(data).subspan(177);
    const auto combined = axiom::core::crc32_combine(
        axiom::core::crc32(left), axiom::core::crc32(right), right.size());
    AXIOM_CHECK(combined == axiom::core::crc32(data));

    // A large buffer takes the folded (PCLMUL) path where available; walking
    // the same bytes in small prime-sized chunks stays on the scalar path, so
    // this pins the two implementations to each other. The trailing odd sizes
    // exercise every head/tail split around the 16-byte folding granularity.
    data.resize((1u << 20) + 37);
    for (auto& byte : data) {
        byte = static_cast<std::uint8_t>(rng());
    }
    const auto whole = axiom::core::crc32(data);
    AXIOM_CHECK(whole == crc32_reference(data));
    auto chunked = axiom::core::crc32_init();
    for (std::size_t offset = 0; offset < data.size();) {
        const auto step = std::min<std::size_t>(61, data.size() - offset);
        chunked = axiom::core::crc32_update(chunked, std::span(data).subspan(offset, step));
        offset += step;
    }
    AXIOM_CHECK(axiom::core::crc32_final(chunked) == whole);

    // CPU detection must at least run and return a string (content is host-specific).
    AXIOM_CHECK(axiom::core::cpu_features_string() != nullptr);
}

// ---- multi-file archive tests ----------------------------------------------

namespace fs = std::filesystem;

std::vector<std::uint8_t> read_all(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void write_all(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

fs::path make_temp_dir() {
    static int counter = 0;
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto dir = fs::temp_directory_path() /
                     ("axiom_test_" + std::to_string(stamp) + "_" + std::to_string(counter++));
    fs::create_directories(dir);
    return dir;
}

template <typename Fn>
void expect_throws(Fn&& fn) {
    bool threw = false;
    try {
        fn();
    } catch (const std::exception&) {
        threw = true;
    }
    AXIOM_CHECK(threw);
}

void test_sequence_stream_truncation() {
    const auto input = bytes_from_string(
        "sequence stream truncation should never synthesize missing entropy bytes");
    const auto lz77 = axiom::codec::encode_lz77(input, {});
    auto encoded = axiom::codec::encode_lz77_sequence_streams(input, lz77, false);
    AXIOM_CHECK(encoded.has_value() && !encoded->empty());
    encoded->pop_back();
    expect_throws([&] {
        (void)axiom::codec::decode_lz77_sequence_streams(*encoded, input.size());
    });
}

std::size_t find_bytes(const std::vector<std::uint8_t>& haystack, const std::string& needle) {
    const auto* first = reinterpret_cast<const std::uint8_t*>(needle.data());
    auto it = std::search(haystack.begin(), haystack.end(), first, first + needle.size());
    AXIOM_CHECK(it != haystack.end());
    return static_cast<std::size_t>(it - haystack.begin());
}

std::uint16_t test_read_le16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    AXIOM_CHECK(offset + 2 <= bytes.size());
    return static_cast<std::uint16_t>(bytes[offset] |
                                      (static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
}

std::uint32_t test_read_le32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    AXIOM_CHECK(offset + 4 <= bytes.size());
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

void test_write_le16(std::vector<std::uint8_t>& bytes, std::size_t offset,
                     std::uint16_t value) {
    AXIOM_CHECK(offset + 2 <= bytes.size());
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void test_write_le32(std::vector<std::uint8_t>& bytes, std::size_t offset,
                     std::uint32_t value) {
    AXIOM_CHECK(offset + 4 <= bytes.size());
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

std::vector<std::uint8_t> synthetic_x64_image() {
    std::vector<std::uint8_t> image(768u << 10, 0x90);
    image[0] = 'M';
    image[1] = 'Z';
    test_write_le32(image, 0x3c, 0x80);
    image[0x80] = 'P';
    image[0x81] = 'E';
    image[0x82] = 0;
    image[0x83] = 0;
    test_write_le16(image, 0x84, 0x8664);
    constexpr std::uint32_t target = 0x00402000;
    for (std::size_t pos = 512; pos + 16 <= image.size(); pos += 16) {
        image[pos] = 0xe8;
        test_write_le32(image, pos + 1,
                        target - static_cast<std::uint32_t>(pos + 5));
        image[pos + 5] = 0x48;
        image[pos + 6] = 0x8b;
        image[pos + 7] = 0xc1;
    }
    return image;
}

std::vector<std::uint8_t> synthetic_pcm_wave() {
    constexpr std::size_t frames = 128u << 10;
    std::vector<std::uint8_t> wave(44 + frames * 4, 0);
    wave[0] = 'R'; wave[1] = 'I'; wave[2] = 'F'; wave[3] = 'F';
    test_write_le32(wave, 4, static_cast<std::uint32_t>(wave.size() - 8));
    wave[8] = 'W'; wave[9] = 'A'; wave[10] = 'V'; wave[11] = 'E';
    wave[12] = 'f'; wave[13] = 'm'; wave[14] = 't'; wave[15] = ' ';
    test_write_le32(wave, 16, 16);
    test_write_le16(wave, 20, 1);       // PCM
    test_write_le16(wave, 22, 2);       // stereo
    test_write_le32(wave, 24, 48000);
    test_write_le32(wave, 28, 48000 * 4);
    test_write_le16(wave, 32, 4);       // interleaved frame stride
    test_write_le16(wave, 34, 16);
    wave[36] = 'd'; wave[37] = 'a'; wave[38] = 't'; wave[39] = 'a';
    test_write_le32(wave, 40, static_cast<std::uint32_t>(frames * 4));
    for (std::size_t frame = 0; frame < frames; ++frame) {
        test_write_le16(wave, 44 + frame * 4,
                        static_cast<std::uint16_t>(frame * 3));
        test_write_le16(wave, 46 + frame * 4,
                        static_cast<std::uint16_t>(frame * 5));
    }
    return wave;
}

void test_reversible_transforms_and_v7() {
    auto source = synthetic_x64_image();
    AXIOM_CHECK(axiom::codec::detect_transform_hint(source).transform ==
                axiom::CompressionTransform::x86_branch);
    std::vector<axiom::CompressionTransformRange> ranges{
        {axiom::CompressionTransform::x86_branch, 512,
         static_cast<std::uint64_t>(source.size() - 512), 512, 0},
    };
    const auto normalized = axiom::codec::normalize_transform_ranges(ranges, source.size());
    const auto metadata = axiom::codec::serialize_transform_ranges(normalized);
    const auto decoded_ranges =
        axiom::codec::deserialize_transform_ranges(metadata, source.size());
    AXIOM_CHECK(decoded_ranges.size() == 1);
    auto transformed = axiom::codec::apply_transform_ranges(source, decoded_ranges);
    AXIOM_CHECK(transformed != source);
    axiom::codec::inverse_transform_ranges(transformed, decoded_ranges);
    AXIOM_CHECK(transformed == source);

    std::vector<std::uint8_t> delta_source(32u << 10);
    for (std::size_t i = 0; i < delta_source.size(); ++i) {
        delta_source[i] = static_cast<std::uint8_t>((i / 4 + (i % 4) * 17) & 0xff);
    }
    const std::vector<axiom::CompressionTransformRange> delta_ranges{
        {axiom::CompressionTransform::delta, 0,
         static_cast<std::uint64_t>(delta_source.size()), 0, 4},
    };
    auto delta = axiom::codec::apply_transform_ranges(delta_source, delta_ranges);
    AXIOM_CHECK(delta != delta_source);
    axiom::codec::inverse_transform_ranges(delta, delta_ranges);
    AXIOM_CHECK(delta == delta_source);

    // A smooth 512-sample-wide 16-bit field should select the v7 numeric
    // predictor without relying on its file name. The transform combines a 2D
    // predictor, signed-residual zigzag, and byte-plane shuffling.
    constexpr std::size_t numeric_width = 512;
    constexpr std::size_t numeric_height = 512;
    std::vector<std::uint8_t> numeric(numeric_width * numeric_height * 2);
    for (std::size_t y = 0; y < numeric_height; ++y) {
        for (std::size_t x = 0; x < numeric_width; ++x) {
            test_write_le16(numeric, (y * numeric_width + x) * 2,
                            static_cast<std::uint16_t>(y * 17 + x * 11 +
                                                       ((x ^ y) & 3)));
        }
    }
    const std::vector<axiom::CompressionTransformRange> numeric_ranges{{
        axiom::CompressionTransform::word16_predict, 0,
        static_cast<std::uint64_t>(numeric.size()), 0, 9}};
    auto predicted = axiom::codec::apply_transform_ranges(numeric, numeric_ranges);
    AXIOM_CHECK(predicted != numeric);
    axiom::codec::inverse_transform_ranges(predicted, numeric_ranges);
    AXIOM_CHECK(predicted == numeric);

    // The direct-stream detector also looks through validated POSIX tar headers
    // so a benchmark tar receives the same per-file filters as an AXAR input.
    constexpr std::size_t tar_block = 512;
    const auto padded_numeric = (numeric.size() + tar_block - 1) & ~(tar_block - 1);
    std::vector<std::uint8_t> tar(tar_block + padded_numeric + tar_block * 2, 0);
    std::copy_n("image.raw", 9, tar.begin());
    auto write_octal = [&](std::size_t offset, std::size_t width, std::uint64_t value) {
        std::fill_n(tar.begin() + static_cast<std::ptrdiff_t>(offset), width, '0');
        tar[offset + width - 1] = 0;
        for (std::size_t i = offset + width - 1; i-- > offset && value != 0;) {
            tar[i] = static_cast<std::uint8_t>('0' + (value & 7u));
            value >>= 3;
        }
    };
    write_octal(124, 12, numeric.size());
    std::fill_n(tar.begin() + 148, 8, ' ');
    tar[156] = '0';
    std::copy_n("ustar", 5, tar.begin() + 257);
    std::uint64_t tar_checksum = 0;
    for (std::size_t i = 0; i < tar_block; ++i) {
        tar_checksum += tar[i];
    }
    write_octal(148, 8, tar_checksum);
    std::copy(numeric.begin(), numeric.end(), tar.begin() + tar_block);
    const auto tar_ranges = axiom::codec::detect_transform_ranges(tar);
    AXIOM_CHECK(tar_ranges.size() == 1);
    AXIOM_CHECK(tar_ranges[0].transform ==
                axiom::CompressionTransform::word16_predict);
    AXIOM_CHECK(tar_ranges[0].offset == tar_block);
    AXIOM_CHECK(tar_ranges[0].size == numeric.size());

    axiom::CompressionOptions options;
    options.thread_count = 1;
    options.block_size = 1u << 20;
    const auto filtered = axiom::compress(source, options);
    options.enable_file_filters = false;
    const auto plain = axiom::compress(source, options);
    AXIOM_CHECK(test_read_le16(filtered, 8) == 7);
    AXIOM_CHECK((filtered[11] & 1u) != 0);
    AXIOM_CHECK(test_read_le32(filtered, 32) != 0);
    AXIOM_CHECK(filtered.size() < plain.size());
    AXIOM_CHECK(axiom::decompress(filtered) == source);

    // A useful transformed file can occupy only a small middle range of a large
    // solid block. The trial must sample that range instead of missing it while
    // sampling unrelated bytes from the block.
    std::vector<std::uint8_t> sparse_block(8u << 20);
    std::uint32_t noise = 0x12345678u;
    for (auto& byte : sparse_block) {
        noise = noise * 1664525u + 1013904223u;
        byte = static_cast<std::uint8_t>(noise >> 24);
    }
    constexpr std::size_t sparse_offset = (3u << 20) + (256u << 10);
    std::copy(source.begin(), source.end(), sparse_block.begin() + sparse_offset);
    axiom::CompressionOptions sparse_options;
    sparse_options.thread_count = 1;
    sparse_options.transform_ranges = {{
        axiom::CompressionTransform::x86_branch,
        sparse_offset,
        static_cast<std::uint64_t>(source.size()),
        0,
        0,
    }};
    const auto sparse_filtered = axiom::compress(sparse_block, sparse_options);
    AXIOM_CHECK((sparse_filtered[11] & 1u) != 0);
    AXIOM_CHECK(axiom::decompress(sparse_filtered) == sparse_block);

    const auto wave = synthetic_pcm_wave();
    axiom::CompressionOptions wave_options;
    wave_options.thread_count = 1;
    const auto filtered_wave = axiom::compress(wave, wave_options);
    wave_options.enable_file_filters = false;
    const auto plain_wave = axiom::compress(wave, wave_options);
    AXIOM_CHECK((filtered_wave[11] & 1u) != 0);
    AXIOM_CHECK(filtered_wave.size() < plain_wave.size());
    AXIOM_CHECK(axiom::decompress(filtered_wave) == wave);

    auto bad_flag = filtered;
    bad_flag[11] = 0;
    expect_throws([&] { (void)axiom::decompress(bad_flag); });
    auto bad_size = filtered;
    test_write_le32(bad_size, 32, 0xffffffffu);
    expect_throws([&] { (void)axiom::decompress(bad_size); });
    auto bad_transform = filtered;
    bad_transform[37] = 0xff;  // metadata starts with count, then transform id
    expect_throws([&] { (void)axiom::decompress(bad_transform); });

    // Version 4 used the same 32-byte fixed header without the v5 metadata-size
    // field. New decoders continue to accept a canonical legacy stream.
    auto legacy = plain;
    AXIOM_CHECK(legacy[11] == 0 && test_read_le32(legacy, 32) == 0);
    legacy.erase(legacy.begin() + 32, legacy.begin() + 36);
    test_write_le16(legacy, 8, 4);
    AXIOM_CHECK(axiom::decompress(legacy) == source);

    // Version 5 has the same extended header as v6/v7. Keep accepting its
    // original codec set while reserving newer block codecs for later streams.
    axiom::CompressionOptions store_options;
    store_options.force_store = true;
    auto previous = axiom::compress(source, store_options);
    test_write_le16(previous, 8, 5);
    AXIOM_CHECK(axiom::decompress(previous) == source);

    const std::vector<axiom::CompressionTransformRange> overlapping{
        {axiom::CompressionTransform::delta, 0, 8192, 0, 1},
        {axiom::CompressionTransform::delta, 4096, 8192, 0, 1},
    };
    expect_throws([&] {
        (void)axiom::codec::normalize_transform_ranges(overlapping, 16384);
    });
}

void test_filtered_axar_roundtrip() {
    const auto root = make_temp_dir();
    const auto executable_one = root / "one.exe";
    const auto executable_two = root / "two.dll";
    const auto text_path = root / "notes.txt";
    auto second_image = synthetic_x64_image();
    second_image[0x100] ^= 0x5a;
    write_all(executable_one, synthetic_x64_image());
    write_all(executable_two, second_image);
    write_all(text_path, bytes_from_string(std::string(96u << 10, 'T')));

    axiom::CompressionOptions options;
    options.thread_count = 1;
    options.block_size = 4u << 20;
    options.auto_block_size_for_threads = false;
    const auto filtered_archive = root / "filtered.axar";
    const auto plain_archive = root / "plain.axar";
    axiom::create_archive({executable_one, text_path, executable_two},
                          filtered_archive, options);
    options.enable_file_filters = false;
    axiom::create_archive({executable_one, text_path, executable_two},
                          plain_archive, options);
    AXIOM_CHECK(fs::file_size(filtered_archive) < fs::file_size(plain_archive));
    axiom::test_archive(filtered_archive);

    axiom::CompressionEstimateOptions estimate_options;
    estimate_options.compression = options;
    estimate_options.compression.enable_file_filters = true;
    estimate_options.sample_budget = 1u << 20;
    estimate_options.sample_chunk_size = 256u << 10;
    estimate_options.time_budget = std::chrono::seconds(10);
    const auto filtered_estimate =
        axiom::estimate_compression({executable_one}, estimate_options);
    estimate_options.compression.enable_file_filters = false;
    const auto plain_estimate =
        axiom::estimate_compression({executable_one}, estimate_options);
    AXIOM_CHECK(filtered_estimate.estimated_archive_bytes <
                plain_estimate.estimated_archive_bytes);

    const auto destination = root / "out";
    axiom::ExtractOptions extraction;
    extraction.overwrite = axiom::ExtractOptions::Overwrite::overwrite;
    axiom::extract_archive(filtered_archive, destination, extraction);
    AXIOM_CHECK(read_all(destination / "one.exe") == read_all(executable_one));
    AXIOM_CHECK(read_all(destination / "two.dll") == read_all(executable_two));
    AXIOM_CHECK(read_all(destination / "notes.txt") == read_all(text_path));

    std::error_code ec;
    fs::remove_all(root, ec);
}

void test_archive_roundtrip() {
    const auto root = make_temp_dir();
    const auto src = root / "src";
    fs::create_directories(src / "sub" / "deep");
    fs::create_directories(src / "emptydir");

    const auto text = bytes_from_string(std::string("hello world\n") + std::string(5000, 'a'));
    std::vector<std::uint8_t> binary(40000);
    for (std::size_t i = 0; i < binary.size(); ++i) {
        binary[i] = static_cast<std::uint8_t>((i * 37 + (i >> 6)) & 0xFF);
    }
    write_all(src / "readme.txt", text);
    write_all(src / "sub" / "data.bin", binary);
    write_all(src / "sub" / "deep" / "note.md", bytes_from_string("# note\n"));
    write_all(src / "empty.dat", {});

    const auto archive = root / "out.axar";
    axiom::CompressionOptions options;
    options.block_size = 16 * 1024;  // small, to force the big file to span blocks
    axiom::create_archive({src}, archive, options);

    const auto entries = axiom::list_archive(archive);
    // 4 directories (src, src/sub, src/sub/deep, src/emptydir) + 4 files.
    AXIOM_CHECK(entries.size() == 8);
    axiom::test_archive(archive);

    const auto dest = root / "out";
    axiom::extract_archive(archive, dest, {});

    AXIOM_CHECK(read_all(dest / "src" / "readme.txt") == text);
    AXIOM_CHECK(read_all(dest / "src" / "sub" / "data.bin") == binary);
    AXIOM_CHECK(read_all(dest / "src" / "sub" / "deep" / "note.md") == bytes_from_string("# note\n"));
    AXIOM_CHECK(fs::exists(dest / "src" / "empty.dat"));
    AXIOM_CHECK(fs::file_size(dest / "src" / "empty.dat") == 0);
    AXIOM_CHECK(fs::is_directory(dest / "src" / "emptydir"));

    std::error_code ec;
    fs::remove_all(root, ec);
}

void test_archive_provider_layer() {
    const auto root = make_temp_dir();
    const auto src = root / "src";
    fs::create_directories(src);
    write_all(src / "file.txt", bytes_from_string("provider extraction"));

    const auto archive = root / "provider.AXAR";
    axiom::create_archive({src}, archive, {});

    const auto formats = axiom::supported_archive_formats();
    AXIOM_CHECK(!formats.empty());
    AXIOM_CHECK(formats.front().id == "axar");

    const auto* provider = axiom::archive_provider_for_path(archive);
    AXIOM_CHECK(provider != nullptr);
    AXIOM_CHECK(axiom::archive_provider_for_contents(archive) == provider);
    AXIOM_CHECK(provider->info().native);
    AXIOM_CHECK(axiom::is_supported_archive(archive));
    AXIOM_CHECK(axiom::is_native_archive(archive));
    AXIOM_CHECK(!axiom::is_supported_archive(root / "not.unsupported"));

    const auto capabilities = provider->capabilities(archive);
    AXIOM_CHECK(capabilities.list);
    AXIOM_CHECK(capabilities.extract);
    AXIOM_CHECK(capabilities.test);
    AXIOM_CHECK(capabilities.create);
    AXIOM_CHECK(capabilities.packed_sizes);
    AXIOM_CHECK(capabilities.selective_extract);
    AXIOM_CHECK(capabilities.update);
    AXIOM_CHECK(capabilities.comments);
    AXIOM_CHECK(capabilities.sfx);
    AXIOM_CHECK(!capabilities.encrypted);
    AXIOM_CHECK(!capabilities.directory_encrypted);

    // Capability queries must not throw for archives that do not exist yet
    // (the GUI probes the target path before creating a new archive) and must
    // still allow creation; archive-state flags stay at their defaults.
    {
        const auto missing_axar = root / "missing" / "new.axar";
        const auto* missing_provider = axiom::archive_provider_for_path(missing_axar);
        AXIOM_CHECK(missing_provider != nullptr);
        AXIOM_CHECK(axiom::archive_provider_for_contents(missing_axar) == nullptr);
        const auto missing_capabilities = missing_provider->capabilities(missing_axar);
        AXIOM_CHECK(missing_capabilities.create);
        AXIOM_CHECK(missing_capabilities.update);
        AXIOM_CHECK(!missing_capabilities.encrypted);
        AXIOM_CHECK(!missing_capabilities.locked);
        AXIOM_CHECK(!missing_capabilities.directory_encrypted);

        const auto missing_zip = root / "missing" / "new.zip";
        const auto* zip_provider = axiom::archive_provider_for_path(missing_zip);
        AXIOM_CHECK(zip_provider != nullptr);
        const auto zip_capabilities = zip_provider->capabilities(missing_zip);
        AXIOM_CHECK(zip_capabilities.create);
        AXIOM_CHECK(!zip_capabilities.encrypted);
    }

    // Content detection is extension-independent and never promotes a filename
    // alone. The GUI tree uses this distinction for its expandable archive nodes.
    {
        const auto renamed = root / "renamed-archive.data";
        fs::copy_file(archive, renamed);
        const auto* renamed_provider = axiom::archive_provider_for_contents(renamed);
        AXIOM_CHECK(renamed_provider != nullptr);
        AXIOM_CHECK(renamed_provider->info().format == axiom::ArchiveFormat::axar);

        const auto fake_zip = root / "ordinary-file.zip";
        write_all(fake_zip, bytes_from_string("not an archive"));
        AXIOM_CHECK(axiom::archive_provider_for_contents(fake_zip) == nullptr);
        AXIOM_CHECK(axiom::archive_provider_for_path(fake_zip) != nullptr);
    }

    const auto entries = provider->list(archive);
    AXIOM_CHECK(std::any_of(entries.begin(), entries.end(), [](const axiom::ArchiveEntry& entry) {
        return entry.path == "src/file.txt" &&
               entry.packed_size.has_value() &&
               entry.packed_size_estimated;
    }));
    provider->test(archive);

    const auto selected = root / "selected";
    provider->extract_selected(archive, {"src/file.txt"}, selected, {});
    AXIOM_CHECK(read_all(selected / "src" / "file.txt") == bytes_from_string("provider extraction"));

    const auto full = root / "full";
    provider->extract_all(archive, full, {});
    AXIOM_CHECK(read_all(full / "src" / "file.txt") == bytes_from_string("provider extraction"));

    std::error_code ec;
    fs::remove_all(root, ec);
}

void create_test_zip_archive(const fs::path& archive) {
    mz_zip_archive zip{};
    mz_zip_zero_struct(&zip);
    AXIOM_CHECK(mz_zip_writer_init_file(&zip, archive.string().c_str(), 0));

    const char* first = "zip provider extraction";
    const char* second = "nested zip payload";
    AXIOM_CHECK(mz_zip_writer_add_mem(&zip, "folder/file.txt", first,
                                      std::strlen(first), MZ_BEST_COMPRESSION));
    AXIOM_CHECK(mz_zip_writer_add_mem(&zip, "folder/sub/data.bin", second,
                                      std::strlen(second), MZ_BEST_SPEED));
    AXIOM_CHECK(mz_zip_writer_finalize_archive(&zip));
    AXIOM_CHECK(mz_zip_writer_end(&zip));
}

void test_zip_provider_layer() {
    const auto root = make_temp_dir();
    const auto archive = root / "sample.ZIP";
    create_test_zip_archive(archive);

    const auto* provider = axiom::archive_provider_for_path(archive);
    AXIOM_CHECK(provider != nullptr);
    AXIOM_CHECK(provider->info().format == axiom::ArchiveFormat::zip);
    AXIOM_CHECK(provider->info().id == "zip");
    AXIOM_CHECK(!provider->info().native);
    AXIOM_CHECK(axiom::is_supported_archive(archive));
    AXIOM_CHECK(!axiom::is_native_archive(archive));

    const auto capabilities = provider->capabilities(archive);
    AXIOM_CHECK(capabilities.list);
    AXIOM_CHECK(capabilities.extract);
    AXIOM_CHECK(capabilities.test);
    AXIOM_CHECK(capabilities.selective_extract);
    AXIOM_CHECK(capabilities.packed_sizes);
    AXIOM_CHECK(capabilities.create);
    AXIOM_CHECK(capabilities.update);
    AXIOM_CHECK(capabilities.delete_entries);
    AXIOM_CHECK(capabilities.move_entries);
    AXIOM_CHECK(capabilities.sfx);

    const auto entries = provider->list(archive);
    AXIOM_CHECK(entries.size() == 2);
    AXIOM_CHECK(std::any_of(entries.begin(), entries.end(), [](const axiom::ArchiveEntry& entry) {
        return entry.path == "folder/file.txt" && !entry.is_directory &&
               entry.size == std::strlen("zip provider extraction") &&
               entry.packed_size.has_value();
    }));
    AXIOM_CHECK(std::any_of(entries.begin(), entries.end(), [](const axiom::ArchiveEntry& entry) {
        return entry.path == "folder/sub/data.bin" && !entry.is_directory &&
               entry.size == std::strlen("nested zip payload") &&
               entry.packed_size.has_value();
    }));

    provider->test(archive);

    const auto selected = root / "selected";
    provider->extract_selected(archive, {"folder/file.txt"}, selected, {});
    AXIOM_CHECK(read_all(selected / "folder" / "file.txt") ==
                bytes_from_string("zip provider extraction"));
    AXIOM_CHECK(!fs::exists(selected / "folder" / "sub" / "data.bin"));

    const auto full = root / "full";
    provider->extract_all(archive, full, {});
    AXIOM_CHECK(read_all(full / "folder" / "file.txt") ==
                bytes_from_string("zip provider extraction"));
    AXIOM_CHECK(read_all(full / "folder" / "sub" / "data.bin") ==
                bytes_from_string("nested zip payload"));

    const auto source = root / "source";
    fs::create_directories(source / "nested");
    write_all(source / "alpha.txt", bytes_from_string("alpha v1"));
    write_all(source / "nested" / "beta.txt", bytes_from_string("beta"));
    const auto writable = root / "writable.zip";
    provider->create({source}, writable, {});
    const auto writable_capabilities = provider->capabilities(writable);
    AXIOM_CHECK(writable_capabilities.can_create_volumes);
    AXIOM_CHECK(!writable_capabilities.is_multi_volume);
    const auto created_out = root / "created";
    provider->extract_all(writable, created_out, {});
    AXIOM_CHECK(read_all(created_out / "source" / "alpha.txt") ==
                bytes_from_string("alpha v1"));
    AXIOM_CHECK(read_all(created_out / "source" / "nested" / "beta.txt") ==
                bytes_from_string("beta"));

#if defined(_WIN32)
    const auto split_zip = root / "split.zip";
    fs::copy_file(writable, split_zip);
    axiom::create_zip_volumes(split_zip, 200);
    AXIOM_CHECK(fs::exists(root / "split.z01"));
    AXIOM_CHECK(axiom::archive_provider_for_path(root / "split.z01") == provider);
    AXIOM_CHECK(provider->list(root / "split.z01").size() >= 2);
    const auto split_capabilities = provider->capabilities(split_zip);
    AXIOM_CHECK(split_capabilities.list);
    AXIOM_CHECK(split_capabilities.extract);
    AXIOM_CHECK(split_capabilities.is_multi_volume);
    AXIOM_CHECK(!split_capabilities.can_create_volumes);
    AXIOM_CHECK(!split_capabilities.update);
    AXIOM_CHECK(!split_capabilities.delete_entries);

    // Cancellation must leave every member of an existing set byte-identical.
    const auto z01_before = read_all(root / "split.z01");
    const auto final_before = read_all(split_zip);
    auto cancelled_split = std::make_shared<axiom::OperationControl>();
    cancelled_split->request_cancel();
    expect_throws([&] {
        axiom::create_zip_volumes(split_zip, 160, cancelled_split);
    });
    AXIOM_CHECK(read_all(root / "split.z01") == z01_before);
    AXIOM_CHECK(read_all(split_zip) == final_before);

    // Cancellation also applies while the direct split reader is moving
    // between disks; no complete temporary ZIP is created.
    axiom::DecompressionOptions cancelled_test_options;
    cancelled_test_options.operation = std::make_shared<axiom::OperationControl>();
    cancelled_test_options.operation->request_cancel();
    expect_throws([&] { provider->test(split_zip, cancelled_test_options); });

    // Even an incomplete set must remain identified as multi-volume/read-only;
    // a failed direct open must not fall back to editable ZIP capabilities.
    fs::remove(root / "split.z01");
    const auto incomplete_capabilities = provider->capabilities(split_zip);
    AXIOM_CHECK(incomplete_capabilities.is_multi_volume);
    AXIOM_CHECK(!incomplete_capabilities.can_create_volumes);
    AXIOM_CHECK(!incomplete_capabilities.update);
    write_all(root / "split.z01", z01_before);

    // Opening a numbered member without the final .zip must fail clearly and
    // must not advertise ordinary editable-ZIP capabilities.
    const auto saved_final = root / "split-final.saved";
    fs::rename(split_zip, saved_final);
    const auto missing_final_capabilities = provider->capabilities(root / "split.z01");
    AXIOM_CHECK(missing_final_capabilities.is_multi_volume);
    AXIOM_CHECK(!missing_final_capabilities.can_create_volumes);
    AXIOM_CHECK(!missing_final_capabilities.update);
    expect_throws([&] { (void)provider->list(root / "split.z01"); });
    fs::rename(saved_final, split_zip);

    // Re-splitting an existing standard set exercises the staged multi-file
    // swap; the previous set remains the source until the new set is complete.
    axiom::create_zip_volumes(split_zip, 240);
    AXIOM_CHECK(fs::exists(root / "split.z01"));
    AXIOM_CHECK(provider->capabilities(split_zip).is_multi_volume);
    const auto split_out = root / "split-out";
    provider->test(split_zip);
    provider->extract_all(split_zip, split_out, {});
    AXIOM_CHECK(read_all(split_out / "source" / "alpha.txt") ==
                bytes_from_string("alpha v1"));
    AXIOM_CHECK(read_all(split_out / "source" / "nested" / "beta.txt") ==
                bytes_from_string("beta"));
    if (const auto seven_zip = bundled_7z_for_tests()) {
        const auto interoperability_out = root / "split-7z-out";
        fs::create_directories(interoperability_out);
        const std::wstring command = L"call \"" + seven_zip->wstring() +
            L"\" x -y -o\"" + interoperability_out.wstring() + L"\" \"" +
            split_zip.wstring() + L"\" >nul 2>nul";
        AXIOM_CHECK(_wsystem(command.c_str()) == 0);
        AXIOM_CHECK(read_all(interoperability_out / "source" / "alpha.txt") ==
                    bytes_from_string("alpha v1"));
        AXIOM_CHECK(read_all(interoperability_out / "source" / "nested" / "beta.txt") ==
                    bytes_from_string("beta"));
    }
#endif


    const auto extra = root / "extra.txt";
    write_all(extra, bytes_from_string("extra payload"));
    provider->add_mapped({axiom::ArchiveInput{extra, "source/extra.txt"}}, writable, {});
    const auto added_out = root / "added";
    provider->extract_all(writable, added_out, {});
    AXIOM_CHECK(read_all(added_out / "source" / "extra.txt") ==
                bytes_from_string("extra payload"));

    write_all(source / "alpha.txt", bytes_from_string("alpha v2"));
    std::error_code time_error;
    fs::last_write_time(source / "alpha.txt",
                        fs::file_time_type::clock::now() + std::chrono::seconds(5),
                        time_error);
    provider->update({source}, writable, {}, false);
    const auto updated_out = root / "updated";
    provider->extract_all(writable, updated_out, {});
    AXIOM_CHECK(read_all(updated_out / "source" / "alpha.txt") ==
                bytes_from_string("alpha v2"));

    provider->move_entries(writable, {axiom::ArchiveMove{"source/nested", "source/moved"}}, {});
    const auto moved_out = root / "moved";
    provider->extract_all(writable, moved_out, {});
    AXIOM_CHECK(read_all(moved_out / "source" / "moved" / "beta.txt") ==
                bytes_from_string("beta"));
    AXIOM_CHECK(!fs::exists(moved_out / "source" / "nested" / "beta.txt"));

    provider->delete_entries(writable, {"source/moved"}, {});
    const auto deleted_out = root / "deleted";
    provider->extract_all(writable, deleted_out, {});
    AXIOM_CHECK(read_all(deleted_out / "source" / "alpha.txt") ==
                bytes_from_string("alpha v2"));
    AXIOM_CHECK(!fs::exists(deleted_out / "source" / "moved" / "beta.txt"));

    std::error_code ec;
    fs::remove_all(root, ec);
}

void test_safe_file_replacement() {
    const auto root = make_temp_dir();
    const auto destination = root / "destination.bin";
    const auto replacement = root / "replacement.bin";
    write_all(destination, bytes_from_string("old bytes"));
    write_all(replacement, bytes_from_string("new bytes"));
    axiom::core::replace_file(replacement, destination, "test file");
    AXIOM_CHECK(read_all(destination) == bytes_from_string("new bytes"));
    AXIOM_CHECK(!fs::exists(replacement));

#if defined(_WIN32)
    write_all(destination, bytes_from_string("preserve me"));
    write_all(replacement, bytes_from_string("must not replace"));
    HANDLE locked = CreateFileW(destination.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    AXIOM_CHECK(locked != INVALID_HANDLE_VALUE);
    expect_throws([&] {
        axiom::core::replace_file(replacement, destination, "locked test file");
    });
    AXIOM_CHECK(read_all(destination) == bytes_from_string("preserve me"));
    AXIOM_CHECK(read_all(replacement) == bytes_from_string("must not replace"));
    CloseHandle(locked);
#endif

    std::error_code ec;
    fs::remove_all(root, ec);
}

#if defined(_WIN32)
void test_skip_unreadable_archive_inputs() {
    const auto root = make_temp_dir();
    const auto source = root / "source";
    fs::create_directories(source);
    write_all(source / "readable.txt", bytes_from_string("keep this file"));
    const auto blocked = source / "blocked.exe";
    write_all(blocked, bytes_from_string("temporarily unavailable"));

    HANDLE lock = CreateFileW(blocked.c_str(), GENERIC_READ, 0, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    AXIOM_CHECK(lock != INVALID_HANDLE_VALUE);

    axiom::CompressionOptions strict;
    strict.input_open_retries = 0;
    expect_throws([&] { axiom::create_archive({source}, root / "strict.axar", strict); });

    axiom::CompressionOptions tolerant;
    tolerant.skip_unreadable_files = true;
    tolerant.input_open_retries = 0;
    tolerant.operation = std::make_shared<axiom::OperationControl>();
    const auto axar = root / "tolerant.axar";
    axiom::create_archive({source}, axar, tolerant);
    const auto axar_entries = axiom::list_archive(axar);
    AXIOM_CHECK(std::any_of(axar_entries.begin(), axar_entries.end(), [](const auto& entry) {
        return entry.path == "source/readable.txt";
    }));
    AXIOM_CHECK(std::none_of(axar_entries.begin(), axar_entries.end(), [](const auto& entry) {
        return entry.path == "source/blocked.exe";
    }));
    const auto axar_warnings = tolerant.operation->warnings();
    AXIOM_CHECK(axar_warnings.size() == 1);
    AXIOM_CHECK(axar_warnings.front().path == "source/blocked.exe");

    const auto zip = root / "tolerant.zip";
    const auto* zip_provider = axiom::archive_provider_for_path(zip);
    AXIOM_CHECK(zip_provider != nullptr);
    auto zip_options = tolerant;
    zip_options.operation = std::make_shared<axiom::OperationControl>();
    zip_provider->create({source}, zip, zip_options);
    const auto zip_entries = zip_provider->list(zip);
    AXIOM_CHECK(std::any_of(zip_entries.begin(), zip_entries.end(), [](const auto& entry) {
        return entry.path == "source/readable.txt";
    }));
    AXIOM_CHECK(std::none_of(zip_entries.begin(), zip_entries.end(), [](const auto& entry) {
        return entry.path == "source/blocked.exe";
    }));
    AXIOM_CHECK(zip_options.operation->warnings().size() == 1);

    CloseHandle(lock);
    std::error_code ec;
    fs::remove_all(root, ec);
}
#endif

#if defined(_WIN32)
bool windows_tar_available() {
    return std::system("tar --version >nul 2>nul") == 0;
}

std::string quote_for_command(const fs::path& path) {
    std::string text = path.string();
    std::string quoted = "\"";
    for (char ch : text) {
        if (ch == '"') quoted += "\\\"";
        else quoted.push_back(ch);
    }
    quoted.push_back('"');
    return quoted;
}

std::optional<fs::path> bundled_7z_for_tests() {
    std::vector<fs::path> roots;
    roots.push_back(fs::current_path());
    roots.push_back(fs::current_path().parent_path());
    roots.push_back(fs::current_path() / "out" / "Release");
    roots.push_back(fs::current_path() / "out" / "Debug");
    for (const auto& root : roots) {
        const fs::path bundled = root / "third_party" / "7zip" / "win-x64" / "7z.exe";
        std::error_code ec;
        if (fs::exists(bundled, ec)) return bundled;
        const fs::path staged = root / "backends" / "7zip" / "7z.exe";
        if (fs::exists(staged, ec)) return staged;
    }
    return std::nullopt;
}

void test_unicode_path_archive_probe() {
    const auto root = make_temp_dir();
    const auto unicode_file = root / L"\u0dc3\u0dd2\u0d82\u0dc4\u0dbd-not-archive.txt";
    write_all(unicode_file, bytes_from_string("not an archive"));

    bool threw = false;
    try {
        AXIOM_CHECK(!axiom::is_supported_archive(unicode_file));
        AXIOM_CHECK(axiom::archive_provider_for_path(unicode_file) == nullptr);
    } catch (...) {
        threw = true;
    }
    AXIOM_CHECK(!threw);

    std::error_code ec;
    fs::remove_all(root, ec);
}

void test_unicode_archive_entry_names() {
    const auto root = make_temp_dir();
    const auto source = root / L"source";
    const auto unicode_directory = source / L"\u0dc3\u0dd2\u0d82\u0dc4\u0dbd";
    const auto unicode_file = unicode_directory / L"\u6587\u4ef6-\U0001f680.txt";
    fs::create_directories(unicode_directory);
    write_all(unicode_file, bytes_from_string("unicode archive entry"));

    const std::string expected = utf8_bytes(
        u8"source/\u0dc3\u0dd2\u0d82\u0dc4\u0dbd/\u6587\u4ef6-\U0001f680.txt");
    const auto contains_expected = [&expected](const std::vector<axiom::ArchiveEntry>& entries) {
        return std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
            return entry.path == expected;
        });
    };

    const auto axar = root / "unicode.axar";
    axiom::create_archive({source}, axar, {});
    AXIOM_CHECK(contains_expected(axiom::list_archive(axar)));
    const auto axar_output = root / "axar-output";
    axiom::extract_archive(axar, axar_output, {});
    AXIOM_CHECK(read_all(axar_output / L"source" / L"\u0dc3\u0dd2\u0d82\u0dc4\u0dbd" /
                         L"\u6587\u4ef6-\U0001f680.txt") ==
                bytes_from_string("unicode archive entry"));

    const auto zip = root / "unicode.zip";
    const auto* zip_provider = axiom::archive_provider_for_path(zip);
    AXIOM_CHECK(zip_provider != nullptr);
    AXIOM_CHECK(zip_provider->info().format == axiom::ArchiveFormat::zip);
    zip_provider->create({source}, zip, {});
    AXIOM_CHECK(contains_expected(zip_provider->list(zip)));
    const auto zip_output = root / "zip-output";
    zip_provider->extract_all(zip, zip_output, {});
    AXIOM_CHECK(read_all(zip_output / L"source" / L"\u0dc3\u0dd2\u0d82\u0dc4\u0dbd" /
                         L"\u6587\u4ef6-\U0001f680.txt") ==
                bytes_from_string("unicode archive entry"));

    // Single-file compression also reports progress using UTF-8 path text.
    const auto single_archive = root / L"\u6587\u4ef6-\U0001f680.axc";
    axiom::CompressionOptions options;
    options.operation = std::make_shared<axiom::OperationControl>();
    axiom::compress_file(unicode_file, single_archive, options);
    AXIOM_CHECK(fs::exists(single_archive));

    std::error_code ec;
    fs::remove_all(root, ec);
}

void test_write_be16(std::vector<std::uint8_t>& bytes, std::size_t offset,
                     std::uint16_t value) {
    AXIOM_CHECK(offset + 2 <= bytes.size());
    bytes[offset] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 1] = static_cast<std::uint8_t>(value);
}

void test_write_be32(std::vector<std::uint8_t>& bytes, std::size_t offset,
                     std::uint32_t value) {
    AXIOM_CHECK(offset + 4 <= bytes.size());
    bytes[offset] = static_cast<std::uint8_t>(value >> 24);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 3] = static_cast<std::uint8_t>(value);
}

std::vector<std::uint8_t> make_iso_directory_record(std::string name,
                                                    std::uint32_t extent,
                                                    std::uint32_t size,
                                                    bool directory) {
    std::size_t record_size = 33 + name.size();
    if ((record_size & 1u) != 0) ++record_size;
    std::vector<std::uint8_t> record(record_size);
    record[0] = static_cast<std::uint8_t>(record_size);
    test_write_le32(record, 2, extent);
    test_write_be32(record, 6, extent);
    test_write_le32(record, 10, size);
    test_write_be32(record, 14, size);
    record[18] = 126;  // 2026
    record[19] = 7;
    record[20] = 5;
    record[21] = 10;
    record[22] = 30;
    record[23] = 0;
    record[24] = 0;
    record[25] = directory ? 0x02 : 0x00;
    test_write_le16(record, 28, 1);
    test_write_be16(record, 30, 1);
    record[32] = static_cast<std::uint8_t>(name.size());
    std::copy(name.begin(), name.end(), record.begin() + 33);
    return record;
}

void put_iso_record(std::vector<std::uint8_t>& image,
                    std::size_t sector,
                    std::size_t& cursor,
                    const std::vector<std::uint8_t>& record) {
    constexpr std::size_t kSectorSize = 2048;
    const std::size_t offset = sector * kSectorSize + cursor;
    AXIOM_CHECK(offset + record.size() <= image.size());
    std::copy(record.begin(), record.end(), image.begin() + static_cast<std::ptrdiff_t>(offset));
    cursor += record.size();
}

void test_iso_native_listing_provider_layer() {
    constexpr std::size_t kSectorSize = 2048;
    constexpr std::uint32_t kRootSector = 18;
    constexpr std::uint32_t kFolderSector = 19;
    constexpr std::uint32_t kHelloSector = 20;
    constexpr std::uint32_t kNestedSector = 21;

    std::vector<std::uint8_t> image(22 * kSectorSize);
    const auto root_record =
        make_iso_directory_record(std::string(1, '\0'), kRootSector,
                                  static_cast<std::uint32_t>(kSectorSize), true);

    const std::size_t primary_offset = 16 * kSectorSize;
    image[primary_offset] = 1;
    std::memcpy(image.data() + primary_offset + 1, "CD001", 5);
    image[primary_offset + 6] = 1;
    std::copy(root_record.begin(), root_record.end(),
              image.begin() + static_cast<std::ptrdiff_t>(primary_offset + 156));

    const std::size_t terminator_offset = 17 * kSectorSize;
    image[terminator_offset] = 255;
    std::memcpy(image.data() + terminator_offset + 1, "CD001", 5);
    image[terminator_offset + 6] = 1;

    std::size_t cursor = 0;
    put_iso_record(image, kRootSector, cursor, root_record);
    put_iso_record(image, kRootSector, cursor,
                   make_iso_directory_record(std::string(1, '\1'), kRootSector,
                                             static_cast<std::uint32_t>(kSectorSize), true));
    put_iso_record(image, kRootSector, cursor,
                   make_iso_directory_record("HELLO.TXT;1", kHelloSector, 5, false));
    put_iso_record(image, kRootSector, cursor,
                   make_iso_directory_record("FOLDER", kFolderSector,
                                             static_cast<std::uint32_t>(kSectorSize), true));

    cursor = 0;
    put_iso_record(image, kFolderSector, cursor,
                   make_iso_directory_record(std::string(1, '\0'), kFolderSector,
                                             static_cast<std::uint32_t>(kSectorSize), true));
    put_iso_record(image, kFolderSector, cursor,
                   make_iso_directory_record(std::string(1, '\1'), kRootSector,
                                             static_cast<std::uint32_t>(kSectorSize), true));
    put_iso_record(image, kFolderSector, cursor,
                   make_iso_directory_record("NESTED.BIN;1", kNestedSector, 4, false));
    std::memcpy(image.data() + kHelloSector * kSectorSize, "hello", 5);
    std::memcpy(image.data() + kNestedSector * kSectorSize, "data", 4);

    const auto root = make_temp_dir();
    const auto archive = root / "disc-image.without_iso_extension";
    write_all(archive, image);

    const auto* provider = axiom::archive_provider_for_path(archive);
    AXIOM_CHECK(provider != nullptr);
    AXIOM_CHECK(provider->info().format == axiom::ArchiveFormat::iso);

    const auto capabilities = provider->capabilities(archive);
    AXIOM_CHECK(capabilities.list);
    AXIOM_CHECK(capabilities.packed_sizes);

    const auto entries = provider->list(archive);
    AXIOM_CHECK(entries.size() == 3);
    const auto hello = std::find_if(entries.begin(), entries.end(),
        [](const axiom::ArchiveEntry& entry) {
            return entry.path == "HELLO.TXT" && !entry.is_directory;
        });
    AXIOM_CHECK(hello != entries.end());
    AXIOM_CHECK(hello->size == 5);
    AXIOM_CHECK(hello->packed_size.has_value() && *hello->packed_size == 5);
    AXIOM_CHECK(!hello->has_crc32);
    AXIOM_CHECK(std::any_of(entries.begin(), entries.end(),
        [](const axiom::ArchiveEntry& entry) {
            return entry.path == "FOLDER" && entry.is_directory;
        }));
    AXIOM_CHECK(std::any_of(entries.begin(), entries.end(),
        [](const axiom::ArchiveEntry& entry) {
            return entry.path == "FOLDER/NESTED.BIN" && !entry.is_directory &&
                   entry.size == 4;
        }));

    std::error_code ec;
    fs::remove_all(root, ec);
}

void test_system_archive_provider_layer() {
    if (!windows_tar_available()) {
        return;
    }

    const auto root = make_temp_dir();
    const auto source = root / "source";
    fs::create_directories(source / "folder");
    write_all(source / "folder" / "hello.txt",
              bytes_from_string("system archive provider payload"));
    write_all(source / "top.txt", bytes_from_string("top-level payload"));

    const auto archive = root / "sample.not_tar";
    const std::string command =
        "tar -cf \"" + archive.string() + "\" -C \"" + source.string() +
        "\" folder top.txt";
    AXIOM_CHECK(std::system(command.c_str()) == 0);

    const auto* provider = axiom::archive_provider_for_path(archive);
    AXIOM_CHECK(provider != nullptr);
    AXIOM_CHECK(provider->info().format == axiom::ArchiveFormat::tar);
    AXIOM_CHECK(!provider->info().native);

    const auto capabilities = provider->capabilities(archive);
    AXIOM_CHECK(capabilities.list);
    AXIOM_CHECK(capabilities.extract);
    AXIOM_CHECK(capabilities.test);
    AXIOM_CHECK(capabilities.selective_extract);
    AXIOM_CHECK(!capabilities.create);
    AXIOM_CHECK(!capabilities.update);
    AXIOM_CHECK(!capabilities.delete_entries);
    AXIOM_CHECK(!capabilities.move_entries);

    const auto entries = provider->list(archive);
    AXIOM_CHECK(std::any_of(entries.begin(), entries.end(), [](const axiom::ArchiveEntry& entry) {
        return entry.path == "folder/hello.txt" && !entry.is_directory &&
               entry.size == std::strlen("system archive provider payload");
    }));

    provider->test(archive);

    const auto selected = root / "selected";
    provider->extract_selected(archive, {"folder/hello.txt"}, selected, {});
    AXIOM_CHECK(read_all(selected / "folder" / "hello.txt") ==
                bytes_from_string("system archive provider payload"));
    AXIOM_CHECK(!fs::exists(selected / "top.txt"));

    const auto full = root / "full";
    provider->extract_all(archive, full, {});
    AXIOM_CHECK(read_all(full / "folder" / "hello.txt") ==
                bytes_from_string("system archive provider payload"));
    AXIOM_CHECK(read_all(full / "top.txt") == bytes_from_string("top-level payload"));

    std::error_code ec;
    fs::remove_all(root, ec);
}

void test_bundled_7z_provider_layer() {
    const auto seven_zip = bundled_7z_for_tests();
    if (!seven_zip) {
        return;
    }

    const auto root = make_temp_dir();
    const auto source = root / "source";
    fs::create_directories(source / "folder");
    const auto alpha = bytes_from_string("encrypted 7z alpha payload");
    const auto beta = bytes_from_string("encrypted 7z beta payload");
    write_all(source / "alpha.txt", alpha);
    write_all(source / "folder" / "beta.txt", beta);

    const auto archive = root / "encrypted.not7z";
    const std::string command =
        "cd /d " + quote_for_command(source) + " && " +
        quote_for_command(*seven_zip) +
        " a -t7z -psecret -mhe=on " + quote_for_command(archive) +
        " alpha.txt folder\\beta.txt >nul";
    AXIOM_CHECK(std::system(command.c_str()) == 0);

    const auto* provider = axiom::archive_provider_for_path(archive);
    AXIOM_CHECK(provider != nullptr);
    AXIOM_CHECK(provider->info().format == axiom::ArchiveFormat::seven_z);

    const auto locked = provider->capabilities(archive);
    AXIOM_CHECK(locked.encryption);
    AXIOM_CHECK(locked.encrypted);
    AXIOM_CHECK(locked.directory_encrypted);

    const auto unlocked = provider->capabilities(archive, "secret");
    AXIOM_CHECK(unlocked.list);
    AXIOM_CHECK(unlocked.extract);
    AXIOM_CHECK(unlocked.test);
    AXIOM_CHECK(unlocked.selective_extract);
    AXIOM_CHECK(unlocked.packed_sizes);
    AXIOM_CHECK(unlocked.encrypted);

    const auto entries = provider->list(archive, "secret");
    const auto alpha_it = std::find_if(entries.begin(), entries.end(),
        [&](const axiom::ArchiveEntry& entry) {
            return entry.path == "alpha.txt";
        });
    AXIOM_CHECK(alpha_it != entries.end());
    AXIOM_CHECK(alpha_it->has_crc32);
    AXIOM_CHECK(alpha_it->crc32 == axiom::core::crc32(alpha));

    axiom::DecompressionOptions test_options;
    test_options.password = "secret";
    provider->test(archive, test_options);

    axiom::ExtractOptions extract_options;
    extract_options.password = "secret";
    const auto selected = root / "selected";
    provider->extract_selected(archive, {"alpha.txt"}, selected, extract_options);
    AXIOM_CHECK(read_all(selected / "alpha.txt") == alpha);
    AXIOM_CHECK(!fs::exists(selected / "folder" / "beta.txt"));

    std::error_code ec;
    fs::remove_all(root, ec);
}
#endif

void test_zip_aes256_encryption() {
    const auto root = make_temp_dir();
    const auto first = root / "alpha.txt";
    const auto second = root / "beta.bin";
    write_all(first, bytes_from_string("zip aes alpha payload"));
    write_all(second, bytes_from_string("zip aes beta payload"));

    const auto archive = root / "encrypted.zip";
    const auto* provider = axiom::archive_provider_for_path(archive);
    AXIOM_CHECK(provider != nullptr);

    axiom::CompressionOptions options;
    options.password = "correct horse battery staple";
    provider->add_mapped({axiom::ArchiveInput{first, "folder/alpha.txt"},
                          axiom::ArchiveInput{second, "folder/beta.bin"}},
                         archive, options);

    mz_zip_archive zip{};
    mz_zip_zero_struct(&zip);
    AXIOM_CHECK(mz_zip_reader_init_file(&zip, archive.string().c_str(), 0));
    mz_zip_archive_file_stat stat{};
    AXIOM_CHECK(mz_zip_reader_file_stat(&zip, 0, &stat));
    AXIOM_CHECK(stat.m_is_encrypted != 0);
    AXIOM_CHECK(stat.m_method == 99);
    AXIOM_CHECK(mz_zip_reader_end(&zip));

    const auto listed = provider->list(archive);
    AXIOM_CHECK(listed.size() == 2);
    AXIOM_CHECK(std::all_of(listed.begin(), listed.end(), [](const axiom::ArchiveEntry& entry) {
        return !entry.is_directory && entry.packed_size.has_value();
    }));

    const auto locked_caps = provider->capabilities(archive);
    AXIOM_CHECK(locked_caps.list);
    AXIOM_CHECK(locked_caps.encryption);
    AXIOM_CHECK(locked_caps.encrypted);
    AXIOM_CHECK(!locked_caps.extract);
    AXIOM_CHECK(!locked_caps.test);
    AXIOM_CHECK(!locked_caps.selective_extract);
    AXIOM_CHECK(!locked_caps.update);

    const auto unlocked_caps = provider->capabilities(archive, options.password);
    AXIOM_CHECK(unlocked_caps.extract);
    AXIOM_CHECK(unlocked_caps.test);
    AXIOM_CHECK(unlocked_caps.selective_extract);
    AXIOM_CHECK(!unlocked_caps.update);

    bool failed_without_password = false;
    try {
        provider->test(archive);
    } catch (const std::exception&) {
        failed_without_password = true;
    }
    AXIOM_CHECK(failed_without_password);

    axiom::DecompressionOptions test_options;
    test_options.password = options.password;
    provider->test(archive, test_options);

    axiom::ExtractOptions wrong_options;
    wrong_options.password = "wrong password";
    bool failed_wrong_password = false;
    try {
        provider->extract_selected(archive, {"folder/alpha.txt"}, root / "wrong",
                                   wrong_options);
    } catch (const std::exception&) {
        failed_wrong_password = true;
    }
    AXIOM_CHECK(failed_wrong_password);

    axiom::ExtractOptions extract_options;
    extract_options.password = options.password;
    const auto selected = root / "selected";
    provider->extract_selected(archive, {"folder/alpha.txt"}, selected, extract_options);
    AXIOM_CHECK(read_all(selected / "folder" / "alpha.txt") ==
                bytes_from_string("zip aes alpha payload"));
    AXIOM_CHECK(!fs::exists(selected / "folder" / "beta.bin"));

    const auto full = root / "full";
    provider->extract_all(archive, full, extract_options);
    AXIOM_CHECK(read_all(full / "folder" / "alpha.txt") ==
                bytes_from_string("zip aes alpha payload"));
    AXIOM_CHECK(read_all(full / "folder" / "beta.bin") ==
                bytes_from_string("zip aes beta payload"));

#if defined(_WIN32)
    const auto split_archive = root / "encrypted-split.zip";
    fs::copy_file(archive, split_archive);
    axiom::create_zip_volumes(split_archive, 160);
    AXIOM_CHECK(fs::exists(root / "encrypted-split.z01"));
    const auto split_caps = provider->capabilities(split_archive, options.password);
    AXIOM_CHECK(split_caps.encrypted);
    AXIOM_CHECK(split_caps.extract);
    AXIOM_CHECK(split_caps.is_multi_volume);
    AXIOM_CHECK(!split_caps.can_create_volumes);
    AXIOM_CHECK(!split_caps.update);
    provider->test(split_archive, test_options);
    const auto split_full = root / "split-full";
    provider->extract_all(split_archive, split_full, extract_options);
    AXIOM_CHECK(read_all(split_full / "folder" / "alpha.txt") ==
                bytes_from_string("zip aes alpha payload"));
    AXIOM_CHECK(read_all(split_full / "folder" / "beta.bin") ==
                bytes_from_string("zip aes beta payload"));
#endif

    std::error_code ec;
    fs::remove_all(root, ec);
}

void add_zip_data_descriptors_for_test(const fs::path& archive) {
    constexpr std::uint32_t kLocal = 0x04034b50u;
    constexpr std::uint32_t kCentral = 0x02014b50u;
    constexpr std::uint32_t kEocd = 0x06054b50u;
    constexpr std::uint32_t kDescriptor = 0x08074b50u;
    constexpr std::uint16_t kEncrypted = 0x0001u;
    constexpr std::uint16_t kDataDescriptor = 0x0008u;
    constexpr std::uint16_t kAesMethod = 99u;

    struct Entry {
        std::size_t central_offset = 0;
        std::uint32_t old_local_offset = 0;
        std::uint32_t new_local_offset = 0;
        std::uint32_t compressed_size = 0;
        std::uint32_t uncompressed_size = 0;
    };

    const auto input = read_all(archive);
    AXIOM_CHECK(input.size() >= 22);
    std::size_t eocd_offset = input.size() - 22;
    while (test_read_le32(input, eocd_offset) != kEocd) {
        AXIOM_CHECK(eocd_offset > 0);
        --eocd_offset;
    }
    AXIOM_CHECK(eocd_offset < input.size() && test_read_le32(input, eocd_offset) == kEocd);
    const std::uint16_t total_entries = test_read_le16(input, eocd_offset + 10);
    const std::uint32_t cd_size = test_read_le32(input, eocd_offset + 12);
    const std::uint32_t cd_offset = test_read_le32(input, eocd_offset + 16);
    AXIOM_CHECK(cd_offset + cd_size <= eocd_offset);

    std::vector<Entry> entries;
    std::size_t pos = cd_offset;
    for (std::uint16_t i = 0; i < total_entries; ++i) {
        AXIOM_CHECK(pos + 46 <= cd_offset + cd_size);
        AXIOM_CHECK(test_read_le32(input, pos) == kCentral);
        const std::uint16_t flags = test_read_le16(input, pos + 8);
        const std::uint16_t method = test_read_le16(input, pos + 10);
        const std::uint16_t name_size = test_read_le16(input, pos + 28);
        const std::uint16_t extra_size = test_read_le16(input, pos + 30);
        const std::uint16_t comment_size = test_read_le16(input, pos + 32);
        AXIOM_CHECK((flags & kEncrypted) != 0);
        AXIOM_CHECK(method == kAesMethod);
        entries.push_back(Entry{pos,
                                test_read_le32(input, pos + 42),
                                0,
                                test_read_le32(input, pos + 20),
                                test_read_le32(input, pos + 24)});
        pos += 46u + name_size + extra_size + comment_size;
    }
    AXIOM_CHECK(pos == cd_offset + cd_size);
    std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) {
        return left.old_local_offset < right.old_local_offset;
    });

    std::vector<std::uint8_t> output;
    output.reserve(input.size() + entries.size() * 16);
    std::size_t cursor = 0;
    for (auto& entry : entries) {
        const std::size_t local_offset = entry.old_local_offset;
        AXIOM_CHECK(local_offset >= cursor && local_offset + 30 <= cd_offset);
        AXIOM_CHECK(test_read_le32(input, local_offset) == kLocal);
        const std::uint16_t name_size = test_read_le16(input, local_offset + 26);
        const std::uint16_t extra_size = test_read_le16(input, local_offset + 28);
        const std::size_t data_offset = local_offset + 30u + name_size + extra_size;
        const std::size_t data_end = data_offset + entry.compressed_size;
        AXIOM_CHECK(data_end <= cd_offset);

        entry.new_local_offset = static_cast<std::uint32_t>(output.size());
        output.insert(output.end(),
                      input.begin() + static_cast<std::ptrdiff_t>(cursor),
                      input.begin() + static_cast<std::ptrdiff_t>(data_end));
        test_write_le16(output, entry.new_local_offset + 6,
                        static_cast<std::uint16_t>(
                            test_read_le16(output, entry.new_local_offset + 6) |
                            kDataDescriptor));
        const std::size_t descriptor_offset = output.size();
        output.resize(output.size() + 16);
        test_write_le32(output, descriptor_offset, kDescriptor);
        test_write_le32(output, descriptor_offset + 4, 0);
        test_write_le32(output, descriptor_offset + 8, entry.compressed_size);
        test_write_le32(output, descriptor_offset + 12, entry.uncompressed_size);
        cursor = data_end;
    }
    output.insert(output.end(),
                  input.begin() + static_cast<std::ptrdiff_t>(cursor),
                  input.begin() + static_cast<std::ptrdiff_t>(cd_offset));

    const std::uint32_t new_cd_offset = static_cast<std::uint32_t>(output.size());
    std::vector<std::uint8_t> central(input.begin() + static_cast<std::ptrdiff_t>(cd_offset),
                                      input.begin() + static_cast<std::ptrdiff_t>(cd_offset + cd_size));
    for (const auto& entry : entries) {
        const std::size_t rel = entry.central_offset - cd_offset;
        test_write_le16(central, rel + 8,
                        static_cast<std::uint16_t>(
                            test_read_le16(central, rel + 8) | kDataDescriptor));
        test_write_le32(central, rel + 42, entry.new_local_offset);
    }
    output.insert(output.end(), central.begin(), central.end());

    std::vector<std::uint8_t> tail(input.begin() + static_cast<std::ptrdiff_t>(eocd_offset),
                                  input.end());
    test_write_le32(tail, 16, new_cd_offset);
    output.insert(output.end(), tail.begin(), tail.end());
    write_all(archive, output);
}

void test_zip_aes_data_descriptor_compatibility() {
    const auto root = make_temp_dir();
    const auto first = root / "alpha.txt";
    const auto second = root / "beta.bin";
    write_all(first, bytes_from_string("zip aes descriptor alpha"));
    write_all(second, bytes_from_string("zip aes descriptor beta"));

    const auto archive = root / "external-style-aes.zip";
    const auto* provider = axiom::archive_provider_for_path(archive);
    AXIOM_CHECK(provider != nullptr);

    axiom::CompressionOptions options;
    options.password = "descriptor password";
    provider->add_mapped({axiom::ArchiveInput{first, "folder/alpha.txt"},
                          axiom::ArchiveInput{second, "folder/beta.bin"}},
                         archive, options);
    add_zip_data_descriptors_for_test(archive);

    const auto listed = provider->list(archive);
    AXIOM_CHECK(listed.size() == 2);
    AXIOM_CHECK(provider->capabilities(archive, options.password).extract);

    axiom::DecompressionOptions test_options;
    test_options.password = options.password;
    provider->test(archive, test_options);

    axiom::ExtractOptions extract_options;
    extract_options.password = options.password;
    const auto out = root / "out";
    provider->extract_all(archive, out, extract_options);
    AXIOM_CHECK(read_all(out / "folder" / "alpha.txt") ==
                bytes_from_string("zip aes descriptor alpha"));
    AXIOM_CHECK(read_all(out / "folder" / "beta.bin") ==
                bytes_from_string("zip aes descriptor beta"));

    std::error_code ec;
    fs::remove_all(root, ec);
}

// Symlinks are archived as links (target stored, not followed) and recreated on
// extract. Creating a symlink can require privilege (Windows without Developer
// Mode), so the test skips itself when the OS refuses rather than failing.
void test_archive_symlinks() {
    const auto root = make_temp_dir();
    const auto src = root / "src";
    fs::create_directories(src);
    write_all(src / "real.txt", bytes_from_string("the real file"));

    std::error_code ec;
    fs::create_symlink("real.txt", src / "link.txt", ec);
    if (ec) {
        // No privilege to create symlinks here; nothing to test.
        fs::remove_all(root, ec);
        return;
    }

    const auto archive = root / "links.axar";
    axiom::create_archive({src}, archive, {});

    const auto entries = axiom::list_archive(archive);
    bool saw_link = false;
    for (const auto& entry : entries) {
        if (entry.path == "src/link.txt") {
            saw_link = true;
            AXIOM_CHECK(entry.is_symlink);
            AXIOM_CHECK(!entry.is_directory);
            AXIOM_CHECK(entry.link_target == "real.txt");
        }
    }
    AXIOM_CHECK(saw_link);
    axiom::test_archive(archive);  // a symlink entry must not break verification

    const auto dest = root / "out";
    axiom::extract_archive(archive, dest, {});
    const auto extracted = dest / "src" / "link.txt";
    AXIOM_CHECK(fs::is_symlink(fs::symlink_status(extracted, ec)));
    AXIOM_CHECK(fs::read_symlink(extracted, ec) == fs::path("real.txt"));
    // The link still resolves to the real content beside it.
    AXIOM_CHECK(read_all(extracted) == bytes_from_string("the real file"));

    const auto selected = root / "selected";
    axiom::extract_entries(archive, {"src/link.txt"}, selected, {});
    AXIOM_CHECK(fs::is_symlink(fs::symlink_status(selected / "src" / "link.txt", ec)));
    AXIOM_CHECK(fs::read_symlink(selected / "src" / "link.txt", ec) == fs::path("real.txt"));
    AXIOM_CHECK(!fs::exists(selected / "src" / "real.txt"));

    fs::remove_all(root, ec);
}

// Two paths to one inode are stored once (canonical file + a hardlink entry) and
// re-linked on extract. Hard links need no privilege on NTFS/most POSIX FSes, so
// this exercises the full path; it skips only if the FS refuses the link.
void test_archive_hardlinks() {
    const auto root = make_temp_dir();
    const auto src = root / "src";
    fs::create_directories(src);
    const auto payload = bytes_from_string("shared inode payload, stored exactly once");
    write_all(src / "a.txt", payload);

    std::error_code ec;
    fs::create_hard_link(src / "a.txt", src / "b.txt", ec);
    if (ec) {
        fs::remove_all(root, ec);
        return;  // filesystem does not support hard links here
    }

    const auto archive = root / "hard.axar";
    axiom::create_archive({src}, archive, {});

    const auto entries = axiom::list_archive(archive);
    int files = 0, hardlinks = 0;
    std::string link_target;
    std::string hardlink_path;
    for (const auto& entry : entries) {
        if (entry.is_hardlink) {
            ++hardlinks;
            link_target = entry.link_target;
            hardlink_path = entry.path;
            AXIOM_CHECK(!entry.is_directory && !entry.is_symlink);
        } else if (!entry.is_directory) {
            ++files;
        }
    }
    // Exactly one of the pair is stored as bytes; the other references it.
    AXIOM_CHECK(files == 1);
    AXIOM_CHECK(hardlinks == 1);
    AXIOM_CHECK(link_target == "src/a.txt" || link_target == "src/b.txt");
    axiom::test_archive(archive);

    const auto dest = root / "out";
    axiom::extract_archive(archive, dest, {});
    AXIOM_CHECK(read_all(dest / "src" / "a.txt") == payload);
    AXIOM_CHECK(read_all(dest / "src" / "b.txt") == payload);
    // The extracted pair should once again share one inode.
    AXIOM_CHECK(fs::hard_link_count(dest / "src" / "a.txt", ec) == 2);

    // Selecting only the hardlink must still produce its bytes without exposing the
    // unselected canonical path in the destination.
    const auto selected = root / "selected";
    axiom::extract_entries(archive, {hardlink_path}, selected, {});
    AXIOM_CHECK(read_all(selected / fs::path(hardlink_path)) == payload);
    AXIOM_CHECK(!fs::exists(selected / fs::path(link_target)));

    const std::string moved_target = "src/canonical-moved.txt";
    axiom::move_archive_entries(archive, {{link_target, moved_target}}, {});
    bool updated_link = false;
    for (const auto& entry : axiom::list_archive(archive)) {
        if (entry.path == hardlink_path) {
            updated_link = entry.is_hardlink && entry.link_target == moved_target;
        }
    }
    AXIOM_CHECK(updated_link);
    const auto moved = root / "moved";
    axiom::extract_archive(archive, moved, {});
    AXIOM_CHECK(read_all(moved / fs::path(moved_target)) == payload);
    AXIOM_CHECK(read_all(moved / fs::path(hardlink_path)) == payload);

    fs::remove_all(root, ec);
}

// add_to_archive appends new files (reusing existing compressed blocks) and an
// added path replaces the existing entry of the same name.
void test_archive_add() {
    const auto root = make_temp_dir();
    const auto src = root / "src";
    fs::create_directories(src);
    const auto a = bytes_from_string("first file content " + std::string(2000, 'x'));
    write_all(src / "a.txt", a);

    const auto archive = root / "a.axar";
    axiom::create_archive({src}, archive, {});

    // Append a brand-new file in a separate directory.
    const auto more = root / "more";
    fs::create_directories(more);
    const auto b = bytes_from_string("second file " + std::string(3000, 'y'));
    write_all(more / "b.txt", b);
    axiom::add_to_archive({more}, archive, {});
    axiom::test_archive(archive);

    {
        bool has_a = false, has_b = false;
        for (const auto& e : axiom::list_archive(archive)) {
            has_a = has_a || e.path == "src/a.txt";
            has_b = has_b || e.path == "more/b.txt";
        }
        AXIOM_CHECK(has_a && has_b);
        const auto dest = root / "out";
        axiom::extract_archive(archive, dest, {});
        AXIOM_CHECK(read_all(dest / "src" / "a.txt") == a);
        AXIOM_CHECK(read_all(dest / "more" / "b.txt") == b);
    }

    // Re-adding src with changed content replaces the existing src/a.txt in place.
    const auto a2 = bytes_from_string("REPLACED content, different length");
    write_all(src / "a.txt", a2);
    axiom::add_to_archive({src}, archive, {});
    axiom::test_archive(archive);

    int count_a = 0;
    for (const auto& e : axiom::list_archive(archive)) {
        count_a += (e.path == "src/a.txt") ? 1 : 0;
    }
    AXIOM_CHECK(count_a == 1);  // replaced, not duplicated

    const auto dest2 = root / "out2";
    axiom::extract_archive(archive, dest2, {});
    AXIOM_CHECK(read_all(dest2 / "src" / "a.txt") == a2);
    AXIOM_CHECK(read_all(dest2 / "more" / "b.txt") == b);  // earlier add untouched

    std::error_code ec;
    fs::remove_all(root, ec);
}

void test_archive_file_manager_apis() {
    const auto root = make_temp_dir();
    const auto src = root / "src";
    fs::create_directories(src / "folder");
    write_all(src / "keep.txt", bytes_from_string("keep"));
    write_all(src / "folder" / "existing.txt", bytes_from_string("existing"));

    const auto archive = root / "drag.axar";
    axiom::create_archive({src}, archive, {});
    AXIOM_CHECK(axiom::archive_encryption_mode(archive) ==
                axiom::ArchiveEncryptionMode::none);

    const auto dropped = root / "dropped.txt";
    const auto dropped_bytes = bytes_from_string("dropped into an archive folder");
    write_all(dropped, dropped_bytes);
    const auto tree = root / "tree";
    fs::create_directories(tree / "deep");
    write_all(tree / "deep" / "leaf.txt", bytes_from_string("leaf"));

    const std::vector<axiom::ArchiveInput> mapped = {
        {dropped, "src/folder/dropped.txt"},
        {tree, "src/folder/tree"},
        {dropped, "virtual/deep/lone.txt"},
    };
    axiom::add_to_archive(mapped, archive, {});
    axiom::test_archive(archive);

    // Invalid, duplicate, and type-changing destinations fail without modifying
    // the existing archive.
    const auto before_invalid = read_all(archive);
    expect_throws([&] {
        axiom::add_to_archive(
            std::vector<axiom::ArchiveInput>{{dropped, "../escape.txt"}}, archive, {});
    });
    expect_throws([&] {
        axiom::add_to_archive(
            std::vector<axiom::ArchiveInput>{{dropped, "src/folder/x.txt"},
                                             {dropped, "src/folder/x.txt"}},
            archive, {});
    });
    expect_throws([&] {
        axiom::add_to_archive(
            std::vector<axiom::ArchiveInput>{{dropped, "src/folder"}}, archive, {});
    });
    AXIOM_CHECK(read_all(archive) == before_invalid);

    // Single-file extraction does not spill unrelated archive entries.
    const auto one = root / "one";
    axiom::extract_entries(archive, {"src/folder/dropped.txt"}, one, {});
    AXIOM_CHECK(read_all(one / "src" / "folder" / "dropped.txt") == dropped_bytes);
    AXIOM_CHECK(!fs::exists(one / "src" / "keep.txt"));
    AXIOM_CHECK(!fs::exists(one / "src" / "folder" / "existing.txt"));

    // Directory selection includes its subtree and overlapping selections are
    // naturally deduplicated.
    const auto subtree = root / "subtree";
    axiom::extract_entries(
        archive, {"src/folder/tree", "src/folder/tree/deep/leaf.txt"}, subtree, {});
    AXIOM_CHECK(read_all(subtree / "src" / "folder" / "tree" / "deep" / "leaf.txt") ==
                bytes_from_string("leaf"));
    AXIOM_CHECK(!fs::exists(subtree / "src" / "keep.txt"));
    const auto implicit = root / "implicit";
    axiom::extract_entries(archive, {"virtual"}, implicit, {});
    AXIOM_CHECK(read_all(implicit / "virtual" / "deep" / "lone.txt") == dropped_bytes);
    expect_throws([&] {
        axiom::extract_entries(archive, {"src/missing.txt"}, root / "missing", {});
    });

    // Moves rewrite only paths/directory metadata; content remains intact.
    axiom::move_archive_entries(
        archive,
        {{"src/folder/dropped.txt", "src/folder/renamed.txt"},
         {"src/folder/tree", "src/moved"}},
        {});
    axiom::move_archive_entries(archive, {{"virtual", "src/implicit"}}, {});
    axiom::test_archive(archive);
    const auto moved = root / "moved-out";
    axiom::extract_entries(archive, {"src/folder/renamed.txt", "src/moved"}, moved, {});
    AXIOM_CHECK(read_all(moved / "src" / "folder" / "renamed.txt") == dropped_bytes);
    AXIOM_CHECK(read_all(moved / "src" / "moved" / "deep" / "leaf.txt") ==
                bytes_from_string("leaf"));
    const auto implicit_moved = root / "implicit-moved";
    axiom::extract_entries(archive, {"src/implicit"}, implicit_moved, {});
    AXIOM_CHECK(read_all(implicit_moved / "src" / "implicit" / "deep" / "lone.txt") ==
                dropped_bytes);

    const auto before_collision = read_all(archive);
    expect_throws([&] {
        axiom::move_archive_entries(archive, {{"src", "src/folder/inside"}}, {});
    });
    expect_throws([&] {
        axiom::move_archive_entries(
            archive, {{"src/folder/renamed.txt", "src/folder/existing.txt"}}, {});
    });
    AXIOM_CHECK(read_all(archive) == before_collision);

    auto cancelled = std::make_shared<axiom::OperationControl>();
    cancelled->request_cancel();
    axiom::CompressionOptions cancelled_options;
    cancelled_options.operation = cancelled;
    expect_throws([&] {
        axiom::add_to_archive(
            std::vector<axiom::ArchiveInput>{{dropped, "src/cancelled.txt"}},
            archive, cancelled_options);
    });
    AXIOM_CHECK(read_all(archive) == before_collision);

    std::error_code ec;
    fs::remove_all(root, ec);
}

// delete removes entries (and directory subtrees) and reclaims their space; repack
// reclaims dead space left behind by a replace.
void test_archive_delete_repack() {
    const auto root = make_temp_dir();
    const auto src = root / "src";
    fs::create_directories(src / "keep");
    const auto big = bytes_from_string(std::string(50000, 'Z'));
    write_all(src / "big.bin", big);
    write_all(src / "keep" / "small.txt", bytes_from_string("keep me"));

    axiom::CompressionOptions opt;
    opt.block_size = 8 * 1024;  // several blocks, so removal visibly shrinks the file
    const auto archive = root / "a.axar";
    axiom::create_archive({src}, archive, opt);
    const auto size_full = fs::file_size(archive);

    // Delete the big file: it disappears and the archive shrinks (space reclaimed).
    axiom::delete_from_archive(archive, {"src/big.bin"}, opt);
    axiom::test_archive(archive);
    {
        bool has_big = false, has_small = false;
        for (const auto& e : axiom::list_archive(archive)) {
            has_big = has_big || e.path == "src/big.bin";
            has_small = has_small || e.path == "src/keep/small.txt";
        }
        AXIOM_CHECK(!has_big);
        AXIOM_CHECK(has_small);
    }
    AXIOM_CHECK(fs::file_size(archive) < size_full);

    const auto dest = root / "out";
    axiom::extract_archive(archive, dest, {});
    AXIOM_CHECK(read_all(dest / "src" / "keep" / "small.txt") == bytes_from_string("keep me"));
    AXIOM_CHECK(!fs::exists(dest / "src" / "big.bin"));

    // Deleting a directory removes its whole subtree.
    axiom::delete_from_archive(archive, {"src/keep"}, opt);
    for (const auto& e : axiom::list_archive(archive)) {
        AXIOM_CHECK(e.path != "src/keep" && e.path != "src/keep/small.txt");
    }

    // Repack reclaims the dead space a same-path replace leaves behind.
    const auto arch2 = root / "b.axar";
    write_all(src / "big.bin", big);
    axiom::create_archive({src}, arch2, opt);
    write_all(src / "big.bin", bytes_from_string(std::string(50000, 'Q')));
    axiom::add_to_archive({src}, arch2, opt);  // replaces big.bin; old blocks linger
    const auto bloated = fs::file_size(arch2);
    axiom::repack_archive(arch2, opt);
    axiom::test_archive(arch2);
    AXIOM_CHECK(fs::file_size(arch2) < bloated);

    const auto dest2 = root / "out2";
    axiom::extract_archive(arch2, dest2, {});
    AXIOM_CHECK(read_all(dest2 / "src" / "big.bin") == bytes_from_string(std::string(50000, 'Q')));

    std::error_code ec;
    fs::remove_all(root, ec);
}

// update adds new + replaces newer (by mtime); fresh refreshes only existing files;
// sync mirrors the inputs, deleting archived entries no longer on disk.
void test_archive_update_sync() {
    const auto root = make_temp_dir();
    const auto src = root / "src";
    fs::create_directories(src);
    write_all(src / "a.txt", bytes_from_string("A original"));

    const auto archive = root / "a.axar";
    axiom::create_archive({src}, archive, {});

    auto has = [&](const std::string& path) {
        for (const auto& e : axiom::list_archive(archive)) {
            if (e.path == path) return true;
        }
        return false;
    };

    // update: a brand-new file is added; the unchanged a.txt is left alone.
    write_all(src / "b.txt", bytes_from_string("B new"));
    axiom::update_archive({src}, archive, {}, /*fresh_only=*/false);
    AXIOM_CHECK(has("src/a.txt") && has("src/b.txt"));

    // A newer a.txt (bumped mtime) is replaced by update.
    write_all(src / "a.txt", bytes_from_string("A UPDATED, different length"));
    std::error_code ec;
    const auto stamp = fs::last_write_time(src / "a.txt", ec);
    fs::last_write_time(src / "a.txt", stamp + std::chrono::seconds(120), ec);
    axiom::update_archive({src}, archive, {}, /*fresh_only=*/false);
    {
        const auto dest = root / "out";
        axiom::extract_archive(archive, dest, {});
        AXIOM_CHECK(read_all(dest / "src" / "a.txt") == bytes_from_string("A UPDATED, different length"));
        AXIOM_CHECK(read_all(dest / "src" / "b.txt") == bytes_from_string("B new"));
    }

    // fresh: a new file is NOT added (fresh only refreshes files already present).
    write_all(src / "c.txt", bytes_from_string("C not added by fresh"));
    axiom::update_archive({src}, archive, {}, /*fresh_only=*/true);
    AXIOM_CHECK(!has("src/c.txt"));

    // sync: c.txt (now on disk) is added; b.txt (removed from disk) is deleted.
    fs::remove(src / "b.txt", ec);
    axiom::sync_archive({src}, archive, {});
    axiom::test_archive(archive);
    AXIOM_CHECK(has("src/c.txt"));
    AXIOM_CHECK(!has("src/b.txt"));

    fs::remove_all(root, ec);
}

// Archive comment survives edits; lock makes every edit operation refuse while
// reads keep working.
void test_archive_comment_lock() {
    const auto root = make_temp_dir();
    const auto src = root / "src";
    fs::create_directories(src);
    write_all(src / "a.txt", bytes_from_string("payload"));
    const auto archive = root / "a.axar";
    axiom::create_archive({src}, archive, {});

    AXIOM_CHECK(axiom::archive_comment(archive).empty());
    AXIOM_CHECK(!axiom::archive_is_locked(archive));

    const std::string note = "release build \xE2\x9C\x93 keep me";  // includes a UTF-8 check mark
    axiom::set_archive_comment(archive, note);
    AXIOM_CHECK(axiom::archive_comment(archive) == note);

    // The comment survives a later edit.
    write_all(src / "b.txt", bytes_from_string("more"));
    axiom::add_to_archive({src}, archive, {});
    AXIOM_CHECK(axiom::archive_comment(archive) == note);
    axiom::test_archive(archive);

    // Locking makes edits refuse; reads still succeed.
    axiom::lock_archive(archive);
    AXIOM_CHECK(axiom::archive_is_locked(archive));
    AXIOM_CHECK(axiom::archive_comment(archive) == note);
    axiom::test_archive(archive);
    AXIOM_CHECK(axiom::list_archive(archive).size() >= 2);
    expect_throws([&] { axiom::add_to_archive({src}, archive, {}); });
    expect_throws([&] { axiom::delete_from_archive(archive, {"src/a.txt"}, {}); });
    expect_throws([&] { axiom::repack_archive(archive, {}); });
    expect_throws([&] {
        axiom::move_archive_entries(archive, {{"src/a.txt", "src/moved.txt"}}, {});
    });
    expect_throws([&] { axiom::set_archive_comment(archive, "nope"); });
    expect_throws([&] { axiom::lock_archive(archive); });

    std::error_code ec;
    fs::remove_all(root, ec);
}

void test_archive_recovery_and_volumes() {
    const auto root = make_temp_dir();
    const auto source = root / "payload.bin";
    std::vector<std::uint8_t> payload(48 * 1024);
    std::mt19937 random(0xA4104u);
    std::generate(payload.begin(), payload.end(), [&] {
        return static_cast<std::uint8_t>(random() & 0xFFu);
    });
    write_all(source, payload);

    axiom::CompressionOptions options;
    options.recovery_percent = 25;
    const auto archive = root / "resilient.axar";
    axiom::create_archive({source}, archive, options);
    auto recovery = axiom::archive_recovery_info(archive);
    AXIOM_CHECK(recovery.present);
    AXIOM_CHECK(recovery.percent == 25);
    AXIOM_CHECK(recovery.parity_shards >= 1);

    // Corrupt protected archive data while leaving the recovery locator intact.
    auto damaged = read_all(archive);
    AXIOM_CHECK(damaged.size() > 64);
    damaged[32] ^= 0xA5u;
    write_all(archive, damaged);
    expect_throws([&] { axiom::test_archive(archive); });
    AXIOM_CHECK(axiom::repair_archive(archive));
    axiom::test_archive(archive);
    {
        const auto extracted = root / "repaired";
        axiom::extract_archive(archive, extracted);
        AXIOM_CHECK(read_all(extracted / "payload.bin") == payload);
    }

    // Ordinary edits preserve an existing recovery percentage unless explicitly
    // replaced or removed.
    const auto added = root / "added.txt";
    write_all(added, bytes_from_string("added after recovery repair"));
    axiom::add_to_archive({added}, archive);
    recovery = axiom::archive_recovery_info(archive);
    AXIOM_CHECK(recovery.present && recovery.percent == 25);
    axiom::set_archive_comment(archive, "recovery survives directory rewrites");
    recovery = axiom::archive_recovery_info(archive);
    AXIOM_CHECK(recovery.present && recovery.percent == 25);
    axiom::test_archive(archive);

    // Two recovery volumes can replace one missing data volume and one data
    // volume whose payload checksum no longer matches its header.
    const auto volume_info = axiom::create_archive_volumes(archive, 4096, 2);
    AXIOM_CHECK(volume_info.data_volumes >= 3);
    AXIOM_CHECK(volume_info.recovery_volumes == 2);
    const auto part1 = root / "resilient.part001.axar";
    const auto part2 = root / "resilient.part002.axar";
    const auto part3 = root / "resilient.part003.axar";
    AXIOM_CHECK(axiom::archive_volume_set_info(part1).data_volumes ==
                volume_info.data_volumes);
    AXIOM_CHECK(axiom::is_axiom_archive_volume(part1));
    AXIOM_CHECK(axiom::archive_volume_data_set_complete(part1));
    AXIOM_CHECK(axiom::archive_volume_primary_path(root / "resilient.rev001") == part1);

    // A complete set is a directly readable, read-only archive. Remove the
    // original to prove list/test/extract do not fall back to or materialize it.
    const auto original_archive_bytes = read_all(archive);
    std::error_code error;
    AXIOM_CHECK(fs::remove(archive, error));
    const auto* volume_provider = axiom::archive_provider_for_contents(part1);
    AXIOM_CHECK(volume_provider != nullptr);
    const auto capabilities = volume_provider->capabilities(part1);
    AXIOM_CHECK(capabilities.list && capabilities.test && capabilities.extract);
    AXIOM_CHECK(capabilities.is_multi_volume);
    AXIOM_CHECK(!capabilities.create && !capabilities.update &&
                !capabilities.delete_entries && !capabilities.move_entries &&
                !capabilities.comments && !capabilities.lock &&
                !capabilities.can_create_volumes);
    const auto direct_entries = volume_provider->list(part1);
    AXIOM_CHECK(direct_entries.size() >= 2);
    volume_provider->test(part1);
    const auto direct_output = root / "direct-volume-output";
    volume_provider->extract_all(part1, direct_output);
    AXIOM_CHECK(read_all(direct_output / "payload.bin") == payload);
    AXIOM_CHECK(read_all(direct_output / "added.txt") ==
                bytes_from_string("added after recovery repair"));
    AXIOM_CHECK(!fs::exists(root / "resilient.axar"));
    expect_throws([&] { axiom::set_archive_comment(part1, "read only"); });

    AXIOM_CHECK(fs::remove(part2, error));
    AXIOM_CHECK(!axiom::archive_volume_data_set_complete(part1));
    expect_throws([&] { (void)axiom::list_archive(part1); });
    auto corrupt_volume = read_all(part1);
    AXIOM_CHECK(corrupt_volume.size() > 80);
    corrupt_volume[80] ^= 0x3Cu;
    write_all(part1, corrupt_volume);

    const auto joined = root / "joined.axar";
    axiom::join_archive_volumes(part3, joined);
    AXIOM_CHECK(read_all(joined) == original_archive_bytes);
    axiom::test_archive(joined);

    axiom::set_archive_recovery(joined, 0);
    AXIOM_CHECK(!axiom::archive_recovery_info(joined).present);
    axiom::test_archive(joined);

    fs::remove_all(root, error);
}

void test_archive_authenticity_and_sfx() {
    const auto root = make_temp_dir();
    const auto source = root / "signed.txt";
    const auto archive = root / "signed.axar";
    write_all(source, bytes_from_string("signed archive payload"));
    axiom::create_archive({source}, archive, {});

    AXIOM_CHECK(!axiom::verify_archive_signature(archive).present);
    const auto key = axiom::generate_archive_signing_key();
    axiom::sign_archive(archive, key);
    const auto verified = axiom::verify_archive_signature(archive, {}, key.public_key);
    AXIOM_CHECK(verified.present);
    AXIOM_CHECK(verified.valid);
    AXIOM_CHECK(verified.trusted_key);
    axiom::test_archive(archive);

    auto tampered = read_all(archive);
    AXIOM_CHECK(tampered.size() > 20);
    tampered[16] ^= 0x40u;
    const auto bad = root / "tampered.axar";
    write_all(bad, tampered);
    const auto rejected = axiom::verify_archive_signature(bad);
    AXIOM_CHECK(rejected.present);
    AXIOM_CHECK(!rejected.valid);
    expect_throws([&] { axiom::test_archive(bad); });

    // Any archive edit deliberately removes the stale signature.
    axiom::set_archive_comment(archive, "changed after signing");
    AXIOM_CHECK(!axiom::verify_archive_signature(archive).present);

    // SFX packaging preserves both the stub and the exact archive, then appends
    // the fixed trailer used by the native GUI self-extractor.
    const auto stub = root / "stub.exe";
    const auto sfx = root / "package.exe";
    const auto stub_bytes = bytes_from_string("MZ fake test stub");
    write_all(stub, stub_bytes);
    const auto archive_bytes = read_all(archive);
    axiom::create_sfx_archive(archive, stub, sfx);
    const auto packaged = read_all(sfx);
    AXIOM_CHECK(packaged.size() == stub_bytes.size() + archive_bytes.size() + 16);
    AXIOM_CHECK(std::equal(stub_bytes.begin(), stub_bytes.end(), packaged.begin()));
    AXIOM_CHECK(std::equal(archive_bytes.begin(), archive_bytes.end(),
                           packaged.begin() + static_cast<std::ptrdiff_t>(stub_bytes.size())));
    const std::string marker = "AXIOMSFX";
    AXIOM_CHECK(std::equal(marker.begin(), marker.end(), packaged.end() - 16));

    std::error_code error;
    fs::remove_all(root, error);
}

// Password-encrypted archives: blocks are sealed, plaintext never hits disk, the
// right password round-trips, and wrong password / tampering are rejected.
void test_archive_encryption() {
    const auto root = make_temp_dir();
    const auto src = root / "src";
    fs::create_directories(src / "sub");
    const auto secret = bytes_from_string("top secret payload " + std::string(3000, 'S'));
    write_all(src / "secret.txt", secret);
    const auto blob = bytes_from_string(std::string(1500, '\x7f'));
    write_all(src / "sub" / "data.bin", blob);

    axiom::CompressionOptions opt;
    opt.block_size = 1024;  // multiple sealed blocks
    opt.password = "hunter2 correct horse";
    const auto archive = root / "enc.axar";
    axiom::create_archive({src}, archive, opt);

    AXIOM_CHECK(axiom::archive_is_encrypted(archive));
    AXIOM_CHECK(axiom::archive_encryption_mode(archive) ==
                axiom::ArchiveEncryptionMode::data_only);
    AXIOM_CHECK(axiom::list_archive(archive).size() >= 2);  // names readable w/o password

    // Wrong or missing password is rejected before any block is read.
    {
        axiom::ExtractOptions x;
        x.password = "wrong password";
        expect_throws([&] { axiom::extract_archive(archive, root / "bad", x); });
        axiom::DecompressionOptions d;  // no password at all
        expect_throws([&] { axiom::test_archive(archive, d); });
    }

    // Correct password round-trips through test + extract.
    {
        axiom::DecompressionOptions d;
        d.password = opt.password;
        axiom::test_archive(archive, d);
        axiom::ExtractOptions x;
        x.password = opt.password;
        const auto dest = root / "out";
        axiom::extract_archive(archive, dest, x);
        AXIOM_CHECK(read_all(dest / "src" / "secret.txt") == secret);
        AXIOM_CHECK(read_all(dest / "src" / "sub" / "data.bin") == blob);
    }

    // The recognizable plaintext marker must not appear anywhere in the archive.
    {
        const auto raw = read_all(archive);
        const std::string marker = "top secret payload";
        const auto* needle = reinterpret_cast<const std::uint8_t*>(marker.data());
        AXIOM_CHECK(std::search(raw.begin(), raw.end(), needle, needle + marker.size()) == raw.end());
    }

    // Tampering with a ciphertext byte must fail authentication, not corrupt silently.
    {
        auto raw = read_all(archive);
        AXIOM_CHECK(raw.size() > 60);
        raw[50] ^= 0xFFu;  // inside the first sealed block
        const auto bad = root / "tampered.axar";
        write_all(bad, raw);
        axiom::DecompressionOptions d;
        d.password = opt.password;
        expect_throws([&] { axiom::test_archive(bad, d); });
    }

    // Editing an encrypted archive: add a file (re-sealed under the same key) with the
    // right password works; a wrong password is rejected before writing.
    {
        write_all(src / "added.txt", bytes_from_string("added under encryption"));
        axiom::CompressionOptions a;
        a.password = "totally wrong";
        const std::vector<axiom::ArchiveInput> encrypted_add = {
            {src / "added.txt", "src/sub/added.txt"}};
        expect_throws([&] { axiom::add_to_archive(encrypted_add, archive, a); });

        a.password = opt.password;
        axiom::add_to_archive(encrypted_add, archive, a);
        AXIOM_CHECK(axiom::archive_is_encrypted(archive));

        axiom::CompressionOptions wrong_move;
        wrong_move.password = "wrong";
        const auto before_wrong_move = read_all(archive);
        expect_throws([&] {
            axiom::move_archive_entries(
                archive, {{"src/sub/added.txt", "src/sub/moved.txt"}}, wrong_move);
        });
        AXIOM_CHECK(read_all(archive) == before_wrong_move);
        axiom::move_archive_entries(
            archive, {{"src/sub/added.txt", "src/sub/moved.txt"}}, a);

        axiom::DecompressionOptions d;
        d.password = opt.password;
        axiom::test_archive(archive, d);  // old and new blocks both verify
        axiom::ExtractOptions x;
        x.password = opt.password;
        const auto dest = root / "out2";
        axiom::extract_archive(archive, dest, x);
        AXIOM_CHECK(read_all(dest / "src" / "sub" / "moved.txt") ==
                    bytes_from_string("added under encryption"));
        AXIOM_CHECK(read_all(dest / "src" / "secret.txt") == secret);  // original intact

        const auto selected = root / "selected-encrypted";
        axiom::ExtractOptions wrong_extract;
        wrong_extract.password = "wrong";
        expect_throws([&] {
            axiom::extract_entries(
                archive, {"src/sub/moved.txt"}, root / "selected-wrong", wrong_extract);
        });
        axiom::extract_entries(archive, {"src/sub/moved.txt"}, selected, x);
        AXIOM_CHECK(read_all(selected / "src" / "sub" / "moved.txt") ==
                    bytes_from_string("added under encryption"));
    }

    // A comment change preserves the encryption metadata (would otherwise orphan the
    // still-encrypted blocks).
    {
        axiom::set_archive_comment(archive, "encrypted backup");
        AXIOM_CHECK(axiom::archive_is_encrypted(archive));
        AXIOM_CHECK(axiom::archive_comment(archive) == "encrypted backup");
        axiom::DecompressionOptions d;
        d.password = opt.password;
        axiom::test_archive(archive, d);
    }

    // Repack rebuilds and re-seals; the archive stays encrypted and readable.
    {
        axiom::CompressionOptions r;
        r.password = opt.password;
        axiom::repack_archive(archive, r);
        axiom::DecompressionOptions d;
        d.password = opt.password;
        axiom::test_archive(archive, d);
        axiom::ExtractOptions x;
        x.password = opt.password;
        const auto dest = root / "out3";
        axiom::extract_archive(archive, dest, x);
        AXIOM_CHECK(read_all(dest / "src" / "secret.txt") == secret);
        AXIOM_CHECK(read_all(dest / "src" / "sub" / "moved.txt") ==
                    bytes_from_string("added under encryption"));
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

// Directory encryption (--encrypt-names): the whole central directory is sealed, so
// names/sizes are hidden and even listing needs the password.
void test_archive_encrypted_directory() {
    const auto root = make_temp_dir();
    const auto src = root / "src";
    fs::create_directories(src);
    const auto secret = bytes_from_string("hidden payload " + std::string(2000, 'H'));
    write_all(src / "secret_name.txt", secret);

    axiom::CompressionOptions opt;
    opt.block_size = 1024;
    opt.password = "directory pass word";
    opt.encrypt_header = true;
    const auto archive = root / "hp.axar";
    axiom::create_archive({src}, archive, opt);

    AXIOM_CHECK(axiom::archive_is_encrypted(archive));  // detectable without a password
    AXIOM_CHECK(axiom::archive_encryption_mode(archive) ==
                axiom::ArchiveEncryptionMode::data_and_directory);

    expect_throws([&] {
        axiom::move_archive_entries(archive, {{"src", "renamed"}}, opt);
    });

    // Listing is refused without (or with a wrong) password — the directory is sealed.
    expect_throws([&] { (void)axiom::list_archive(archive); });
    expect_throws([&] { (void)axiom::list_archive(archive, "wrong"); });

    {
        const auto entries = axiom::list_archive(archive, opt.password);
        bool found = false;
        for (const auto& e : entries) {
            found = found || e.path == "src/secret_name.txt";
        }
        AXIOM_CHECK(found);
    }

    // The file name must not appear in plaintext anywhere in the archive.
    {
        const auto raw = read_all(archive);
        const std::string marker = "secret_name.txt";
        const auto* needle = reinterpret_cast<const std::uint8_t*>(marker.data());
        AXIOM_CHECK(std::search(raw.begin(), raw.end(), needle, needle + marker.size()) == raw.end());
    }

    // Test + extract need the password and reproduce the bytes; wrong password fails.
    {
        axiom::DecompressionOptions d;
        d.password = opt.password;
        axiom::test_archive(archive, d);
        axiom::ExtractOptions x;
        x.password = opt.password;
        const auto dest = root / "out";
        axiom::extract_archive(archive, dest, x);
        AXIOM_CHECK(read_all(dest / "src" / "secret_name.txt") == secret);

        axiom::DecompressionOptions bad;
        bad.password = "nope";
        expect_throws([&] { axiom::test_archive(archive, bad); });
    }

    // Direct volume reads preserve sealed-directory and block encryption. The
    // original archive is absent, so the password-protected directory and data
    // must both be read across numbered part boundaries.
    const auto volume_info = axiom::create_archive_volumes(archive, 300, 1);
    AXIOM_CHECK(volume_info.data_volumes > 1);
    const auto first_part = root / "hp.part001.axar";
    std::error_code remove_error;
    AXIOM_CHECK(fs::remove(archive, remove_error));
    AXIOM_CHECK(axiom::list_archive(first_part, opt.password).size() >= 2);
    axiom::DecompressionOptions direct_test;
    direct_test.password = opt.password;
    axiom::test_archive(first_part, direct_test);
    axiom::ExtractOptions direct_extract;
    direct_extract.password = opt.password;
    const auto direct_dest = root / "volume-out";
    axiom::extract_archive(first_part, direct_dest, direct_extract);
    AXIOM_CHECK(read_all(direct_dest / "src" / "secret_name.txt") == secret);
    expect_throws([&] { (void)axiom::list_archive(first_part, "wrong"); });

    std::error_code ec;
    fs::remove_all(root, ec);
}

void test_archive_operation_control() {
    const auto root = make_temp_dir();
    const auto src = root / "src";
    fs::create_directories(src);

    std::vector<std::uint8_t> payload(256 * 1024);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>('A' + (i % 19));
    }
    write_all(src / "payload.bin", payload);

    const auto archive = root / "progress.axar";
    auto control = std::make_shared<axiom::OperationControl>();
    std::uint64_t max_completed = 0;
    std::uint64_t reported_total = 0;
    bool saw_final = false;
    std::atomic_uint64_t max_compression_file_bytes = 0;
    std::atomic_bool saw_compression_file_path = false;
    std::atomic_bool saw_staging_throughput = false;
    control->set_progress_callback([&](const axiom::OperationProgress& progress) {
        max_completed = std::max(max_completed, progress.completed_bytes);
        if (progress.total_bytes > 0) {
            reported_total = progress.total_bytes;
        }
        if (progress.stage == axiom::OperationStage::finalizing &&
            progress.completed_bytes == progress.total_bytes) {
            saw_final = true;
        }
        if (progress.stage == axiom::OperationStage::compressing &&
            progress.current_file_total_bytes == payload.size()) {
            auto previous = max_compression_file_bytes.load(std::memory_order_relaxed);
            while (progress.current_file_completed_bytes > previous &&
                   !max_compression_file_bytes.compare_exchange_weak(
                       previous, progress.current_file_completed_bytes,
                       std::memory_order_relaxed)) {
            }
            if (progress.current_path.ends_with("payload.bin")) {
                saw_compression_file_path.store(true, std::memory_order_relaxed);
            }
        }
        if (progress.stage == axiom::OperationStage::reading &&
            progress.completed_bytes == 0 && progress.throughput_bytes > 0) {
            saw_staging_throughput.store(true, std::memory_order_relaxed);
        }
    });

    axiom::CompressionOptions options;
    options.block_size = 16 * 1024;
    options.operation = control;
    axiom::create_archive({src}, archive, options);
    AXIOM_CHECK(fs::exists(archive));
    AXIOM_CHECK(reported_total == payload.size());
    AXIOM_CHECK(max_completed == payload.size());
    AXIOM_CHECK(saw_final);
    AXIOM_CHECK(max_compression_file_bytes.load(std::memory_order_relaxed) == payload.size());
    AXIOM_CHECK(saw_compression_file_path.load(std::memory_order_relaxed));
    AXIOM_CHECK(saw_staging_throughput.load(std::memory_order_relaxed));
    const auto final_snapshot = control->latest_progress();
    AXIOM_CHECK(final_snapshot.has_value());
    AXIOM_CHECK(final_snapshot->sequence > 0);
    AXIOM_CHECK(final_snapshot->completed_bytes == final_snapshot->total_bytes);

    // A reader racing a writer must observe one whole telemetry sample, never
    // fields combined from two reports. This is the contract used by the GUI,
    // CLI, and benchmark polling displays.
    auto telemetry = std::make_shared<axiom::OperationControl>();
    std::atomic_bool writer_done = false;
    std::atomic_bool incoherent = false;
    std::jthread writer([&] {
        for (std::uint64_t value = 1; value <= 10000; ++value) {
            telemetry->report(axiom::OperationProgress{
                axiom::OperationStage::compressing, value, 10000,
                value, 10000, std::to_string(value), value, 10000, value});
        }
        writer_done.store(true, std::memory_order_release);
    });
    while (!writer_done.load(std::memory_order_acquire)) {
        const auto snapshot = telemetry->latest_progress();
        if (!snapshot) continue;
        if (snapshot->completed_bytes != snapshot->completed_items ||
            snapshot->completed_bytes != snapshot->current_file_completed_bytes ||
            snapshot->completed_bytes != snapshot->throughput_bytes ||
            snapshot->current_path != std::to_string(snapshot->completed_bytes)) {
            incoherent.store(true, std::memory_order_release);
            break;
        }
    }
    writer.join();
    AXIOM_CHECK(!incoherent.load(std::memory_order_acquire));

    const auto cancelled_archive = root / "cancelled.axar";
    auto cancelled = std::make_shared<axiom::OperationControl>();
    cancelled->request_cancel();
    options.operation = cancelled;
    expect_throws([&] { axiom::create_archive({src}, cancelled_archive, options); });
    AXIOM_CHECK(!fs::exists(cancelled_archive));
    AXIOM_CHECK(!fs::exists(fs::path(cancelled_archive).concat(".tmp")));

    std::error_code ec;
    fs::remove_all(root, ec);
}

void test_compression_estimator() {
    const auto root = make_temp_dir();
    const auto src = root / "estimate-src";
    fs::create_directories(src / "nested");
    write_all(src / "repeated.txt", bytes_from_string(std::string(256 * 1024, 'A')));
    std::vector<std::uint8_t> varied(128 * 1024);
    for (std::size_t index = 0; index < varied.size(); ++index) {
        varied[index] = static_cast<std::uint8_t>((index * 131u + index / 17u) & 0xffu);
    }
    write_all(src / "nested" / "varied.bin", varied);
    write_all(src / "empty.dat", {});

    auto control = std::make_shared<axiom::OperationControl>();
    bool saw_scanning = false;
    bool saw_estimating = false;
    control->set_progress_callback([&](const axiom::OperationProgress& progress) {
        saw_scanning = saw_scanning || progress.stage == axiom::OperationStage::scanning;
        saw_estimating = saw_estimating || progress.stage == axiom::OperationStage::estimating;
    });

    axiom::CompressionEstimateOptions options;
    axiom::apply_compression_level(options.compression, 3);
    options.compression.operation = control;
    options.sample_budget = 1u << 20;
    options.sample_chunk_size = 64u << 10;
    options.time_budget = std::chrono::seconds(10);
    std::uint64_t last_snapshot_bytes = 0;
    std::size_t snapshot_count = 0;
    bool snapshots_monotonic = true;
    axiom::CompressionEstimateSnapshot final_snapshot;
    options.progress_callback = [&](const axiom::CompressionEstimateSnapshot& snapshot) {
        snapshots_monotonic = snapshots_monotonic &&
            snapshot.sampled_bytes >= last_snapshot_bytes;
        last_snapshot_bytes = snapshot.sampled_bytes;
        final_snapshot = snapshot;
        ++snapshot_count;
    };
    const auto estimate = axiom::estimate_compression({src}, options);
    AXIOM_CHECK(estimate.source_bytes == 384u * 1024u);
    AXIOM_CHECK(estimate.sampled_bytes == estimate.source_bytes);
    AXIOM_CHECK(estimate.file_count == 3);
    AXIOM_CHECK(estimate.item_count >= estimate.file_count);
    AXIOM_CHECK(estimate.estimated_archive_bytes > 0);
    AXIOM_CHECK(estimate.estimated_low_bytes <= estimate.estimated_archive_bytes);
    AXIOM_CHECK(estimate.estimated_high_bytes >= estimate.estimated_archive_bytes);
    AXIOM_CHECK(estimate.estimated_ratio > 0.0);
    AXIOM_CHECK(estimate.sample_coverage == 1.0);
    AXIOM_CHECK(saw_scanning);
    AXIOM_CHECK(saw_estimating);
    AXIOM_CHECK(snapshot_count > 1);
    AXIOM_CHECK(snapshots_monotonic);
    AXIOM_CHECK(final_snapshot.sampled_bytes == estimate.sampled_bytes);
    AXIOM_CHECK(final_snapshot.estimated_archive_bytes ==
                estimate.estimated_archive_bytes);

    auto actual_options = options.compression;
    actual_options.operation.reset();
    const auto actual_axar = root / "estimate-actual.axar";
    axiom::create_archive({src}, actual_axar, actual_options);
    const std::uint64_t actual_axar_size = fs::file_size(actual_axar);
    AXIOM_CHECK(estimate.estimated_archive_bytes <= actual_axar_size * 2);
    AXIOM_CHECK(actual_axar_size <= estimate.estimated_archive_bytes * 2);

    auto repeated = options;
    repeated.compression.operation.reset();
    const auto second = axiom::estimate_compression({src}, repeated);
    AXIOM_CHECK(second.estimated_archive_bytes == estimate.estimated_archive_bytes);
    AXIOM_CHECK(second.estimated_low_bytes == estimate.estimated_low_bytes);
    AXIOM_CHECK(second.estimated_high_bytes == estimate.estimated_high_bytes);

    const auto uniform = root / "adaptive-uniform.bin";
    write_all(uniform, std::vector<std::uint8_t>(16u << 20, 0x5a));
    axiom::CompressionEstimateOptions adaptive;
    axiom::apply_compression_level(adaptive.compression, 1);
    adaptive.sample_budget = 8u << 20;
    adaptive.sample_chunk_size = 256u << 10;
    adaptive.time_budget = std::chrono::seconds(20);
    const auto adaptive_estimate = axiom::estimate_compression({uniform}, adaptive);
    AXIOM_CHECK(adaptive_estimate.source_bytes == (16u << 20));
    AXIOM_CHECK(adaptive_estimate.sampled_bytes >= (4u << 20));
    AXIOM_CHECK(adaptive_estimate.sampled_bytes < adaptive.sample_budget);
    AXIOM_CHECK(adaptive_estimate.confidence == axiom::EstimateConfidence::high);
    AXIOM_CHECK(adaptive_estimate.confidence_margin_percent <= 2.5);

    const auto heterogeneous = root / "adaptive-heterogeneous.bin";
    std::vector<std::uint8_t> mixed(16u << 20, 0x41);
    std::uint32_t random = 0x9e3779b9u;
    for (std::size_t index = mixed.size() / 2; index < mixed.size(); ++index) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        mixed[index] = static_cast<std::uint8_t>(random);
    }
    write_all(heterogeneous, mixed);
    const auto heterogeneous_estimate =
        axiom::estimate_compression({heterogeneous}, adaptive);
    AXIOM_CHECK(heterogeneous_estimate.sampled_bytes == adaptive.sample_budget);
    AXIOM_CHECK(heterogeneous_estimate.confidence != axiom::EstimateConfidence::high);
    AXIOM_CHECK(heterogeneous_estimate.confidence_margin_percent > 2.5);

    auto zip = repeated;
    zip.format = axiom::ArchiveFormat::zip;
    zip.volume_size = 64u << 10;
    const auto zip_estimate = axiom::estimate_compression({src}, zip);
    AXIOM_CHECK(zip_estimate.format == axiom::ArchiveFormat::zip);
    AXIOM_CHECK(zip_estimate.estimated_archive_bytes > 0);
    AXIOM_CHECK(zip_estimate.volume_count > 0);
    AXIOM_CHECK(zip_estimate.final_volume_bytes > 0);
    const auto actual_zip = root / "estimate-actual.zip";
    const auto* zip_provider = axiom::archive_provider_for_path(actual_zip);
    AXIOM_CHECK(zip_provider != nullptr);
    zip_provider->create({src}, actual_zip, actual_options);
    const std::uint64_t actual_zip_size = fs::file_size(actual_zip);
    AXIOM_CHECK(zip_estimate.estimated_archive_bytes <= actual_zip_size * 2);
    AXIOM_CHECK(actual_zip_size <= zip_estimate.estimated_archive_bytes * 2);

    const auto missing = axiom::estimate_compression({root / "missing"}, repeated);
    AXIOM_CHECK(missing.source_bytes == 0);
    AXIOM_CHECK(missing.confidence == axiom::EstimateConfidence::low);
    AXIOM_CHECK(missing.warnings.size() == 1);

    auto cancelled = repeated;
    cancelled.compression.operation = std::make_shared<axiom::OperationControl>();
    cancelled.compression.operation->request_cancel();
    expect_throws([&] { (void)axiom::estimate_compression({src}, cancelled); });

    auto cancelled_live = repeated;
    cancelled_live.compression.operation = std::make_shared<axiom::OperationControl>();
    cancelled_live.progress_callback = [operation = cancelled_live.compression.operation](
        const axiom::CompressionEstimateSnapshot&) {
        operation->request_cancel();
    };
    expect_throws([&] { (void)axiom::estimate_compression({src}, cancelled_live); });

    std::error_code error;
    fs::remove_all(root, error);
}

#if defined(_WIN32)
// Windows file attributes and full-precision timestamps must survive a round-trip.
void test_windows_metadata() {
    const auto root = make_temp_dir();
    const auto src = root / "src";
    fs::create_directories(src);
    const auto file = src / "ro.txt";
    write_all(file, bytes_from_string("read-only hidden content"));

    SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN);
    SYSTEMTIME system_time{};
    system_time.wYear = 2021;
    system_time.wMonth = 6;
    system_time.wDay = 15;
    system_time.wHour = 12;
    FILETIME write_time{};
    SystemTimeToFileTime(&system_time, &write_time);
    {
        const HANDLE handle = CreateFileW(file.c_str(), FILE_WRITE_ATTRIBUTES,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                          OPEN_EXISTING, 0, nullptr);
        AXIOM_CHECK(handle != INVALID_HANDLE_VALUE);
        SetFileTime(handle, nullptr, nullptr, &write_time);
        CloseHandle(handle);
    }

    const auto archive = root / "meta.axar";
    axiom::create_archive({src}, archive, {});
    const auto dest = root / "out";
    axiom::extract_archive(archive, dest, {});

    const auto extracted = dest / "src" / "ro.txt";
    WIN32_FILE_ATTRIBUTE_DATA data{};
    AXIOM_CHECK(GetFileAttributesExW(extracted.c_str(), GetFileExInfoStandard, &data));
    AXIOM_CHECK((data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0);
    AXIOM_CHECK((data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0);
    AXIOM_CHECK(data.ftLastWriteTime.dwLowDateTime == write_time.dwLowDateTime &&
                data.ftLastWriteTime.dwHighDateTime == write_time.dwHighDateTime);

    // Clear read-only so the temp tree can be removed.
    SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL);
    SetFileAttributesW(extracted.c_str(), FILE_ATTRIBUTE_NORMAL);
    std::error_code ec;
    fs::remove_all(root, ec);
}

// NTFS alternate data streams (e.g. the Zone.Identifier mark-of-the-web) must be
// captured and recreated on extract.
void test_archive_ads() {
    const auto root = make_temp_dir();
    const auto src = root / "src";
    fs::create_directories(src);
    const auto file = src / "host.txt";
    write_all(file, bytes_from_string("primary content stream"));

    const std::string ads_data = "[ZoneTransfer]\r\nZoneId=3\r\n";
    auto write_stream = [](const std::wstring& path, const std::string& data) {
        const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        AXIOM_CHECK(handle != INVALID_HANDLE_VALUE);
        DWORD wrote = 0;
        WriteFile(handle, data.data(), static_cast<DWORD>(data.size()), &wrote, nullptr);
        CloseHandle(handle);
    };
    auto read_stream = [](const std::wstring& path) {
        const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                          OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return std::string("<missing>");
        }
        char buffer[256];
        DWORD got = 0;
        ReadFile(handle, buffer, sizeof(buffer), &got, nullptr);
        CloseHandle(handle);
        return std::string(buffer, got);
    };

    write_stream(file.wstring() + L":Zone.Identifier", ads_data);

    const auto archive = root / "ads.axar";
    axiom::create_archive({src}, archive, {});
    const auto dest = root / "out";
    axiom::extract_archive(archive, dest, {});

    const auto extracted = dest / "src" / "host.txt";
    AXIOM_CHECK(read_all(extracted) == bytes_from_string("primary content stream"));
    AXIOM_CHECK(read_stream(extracted.wstring() + L":Zone.Identifier") == ads_data);

    std::error_code ec;
    fs::remove_all(root, ec);
}
#endif

void test_archive_safety() {
    const auto root = make_temp_dir();
    const auto src = root / "s";
    fs::create_directories(src);
    write_all(src / "AAAA", bytes_from_string("payload contents that are long enough to fill a block a bit"));

    const auto archive = root / "ok.axar";
    axiom::create_archive({src}, archive, {});
    axiom::test_archive(archive);  // sanity: intact archive passes

    const auto good = read_all(archive);
    const auto bad = root / "bad.axar";

    // Truncated archive.
    {
        auto bytes = good;
        bytes.resize(bytes.size() / 2);
        write_all(bad, bytes);
        expect_throws([&] { (void)axiom::list_archive(bad); });
    }
    // Corrupted header magic.
    {
        auto bytes = good;
        bytes[0] ^= 0xFFu;
        write_all(bad, bytes);
        expect_throws([&] { (void)axiom::list_archive(bad); });
    }
    // Corrupted footer magic.
    {
        auto bytes = good;
        bytes.back() ^= 0xFFu;
        write_all(bad, bytes);
        expect_throws([&] { (void)axiom::list_archive(bad); });
    }
    // Path traversal (zip-slip): patch "s/AAAA" -> equal-length "../zzz".
    {
        auto bytes = good;
        const auto at = find_bytes(bytes, "s/AAAA");
        const std::string evil = "../zzz";
        for (std::size_t i = 0; i < evil.size(); ++i) {
            bytes[at + i] = static_cast<std::uint8_t>(evil[i]);
        }
        write_all(bad, bytes);
        expect_throws([&] { axiom::extract_archive(bad, root / "x", {}); });
        AXIOM_CHECK(!fs::exists(root / "zzz"));  // nothing escaped the destination
    }
    // Corrupted block payload must fail integrity (inner CRC / structure).
    {
        auto bytes = good;
        if (bytes.size() > 40) {
            bytes[40] ^= 0xFFu;
        }
        write_all(bad, bytes);
        expect_throws([&] { axiom::test_archive(bad); });
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

// Safe extraction must refuse to write through a redirecting directory link
// (symlink, or on Windows a junction) sitting in the destination — otherwise a
// pre-existing or archive-planted link could divert files outside the target tree.
// Junctions need no privilege, so this exercises the guard on Windows directly.
void test_extract_link_safety() {
    const auto root = make_temp_dir();
    fs::create_directories(root / "src" / "sub");
    write_all(root / "src" / "sub" / "f.txt", bytes_from_string("must stay inside the destination"));
    const auto archive = root / "a.axar";
    axiom::create_archive({root / "src"}, archive, {});

    const auto dest = root / "out";
    const auto outside = root / "outside";
    fs::create_directories(dest / "src");
    fs::create_directories(outside);

    // Plant a redirecting link where the archive expects a real directory.
    const auto link = dest / "src" / "sub";
    bool linked = false;
#if defined(_WIN32)
    const std::string cmd =
        "cmd /c mklink /J \"" + link.string() + "\" \"" + outside.string() + "\" >nul 2>&1";
    linked = std::system(cmd.c_str()) == 0;
#else
    std::error_code lec;
    fs::create_directory_symlink(outside, link, lec);
    linked = !lec;
#endif
    std::error_code ec;
    if (!linked) {  // platform/filesystem won't make the link; nothing to test
        fs::remove_all(root, ec);
        return;
    }

    // Extraction must refuse rather than follow the link out of the destination.
    expect_throws([&] { axiom::extract_archive(archive, dest, {}); });
    AXIOM_CHECK(!fs::exists(outside / "f.txt"));  // nothing escaped through the link

    fs::remove(link, ec);  // drop the junction before cleaning the tree
    fs::remove_all(root, ec);
}

// A tiny archive that declares an enormous original size must be rejected before
// the decoder tries to expand it (decompression bomb).
void test_decompress_bomb() {
    auto archive = axiom::compress(bytes_from_string(std::string(64, 'a')));
    AXIOM_CHECK(archive.size() > 20);
    // original_size is the u64 at offset 12 (after magic[8], version u16, codec, flags).
    for (int i = 0; i < 8; ++i) {
        archive[12 + i] = 0xFFu;  // ~2^64 declared output
    }
    expect_throws([&] { (void)axiom::decompress(archive); });
    // Decoding never started, so peak memory stayed tiny.
}

void test_split_stream_size_bomb() {
    const std::vector<std::uint8_t> archive{
        0x41, 0x58, 0x49, 0x4f, 0x4d, 0x43, 0x31, 0x00, 0x04, 0x00, 0x05, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x23, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xa2, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0x03, 0x02, 0x0a, 0xdc,
        0xff, 0xb1, 0xc7, 0x61, 0x78, 0x69, 0x6f, 0x6d, 0x31, 0x31, 0x46, 0x6f,
        0x72, 0xa2, 0x04, 0x00, 0x01, 0x00, 0x00,
    };

    axiom::DecompressionOptions options;
    options.max_output_size = std::size_t{16} << 20;
    expect_throws([&] { (void)axiom::decompress(archive, options); });
}

// ---- mutation fuzzing ------------------------------------------------------

void mutate(std::vector<std::uint8_t>& bytes, std::mt19937& rng) {
    if (bytes.empty()) {
        bytes.push_back(static_cast<std::uint8_t>(rng()));
        return;
    }
    const int operations = 1 + static_cast<int>(rng() % 4);
    for (int i = 0; i < operations; ++i) {
        switch (rng() % 4) {
            case 0:
                bytes[rng() % bytes.size()] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
                break;
            case 1:
                bytes[rng() % bytes.size()] = static_cast<std::uint8_t>(rng());
                break;
            case 2:
                if (bytes.size() > 1) {
                    bytes.resize(1 + rng() % bytes.size());
                }
                break;
            default:
                bytes.insert(bytes.begin() + (rng() % (bytes.size() + 1)),
                             static_cast<std::uint8_t>(rng()));
                break;
        }
    }
}

// Feed mutated single-stream archives to the decoder. The contract: it returns a
// value or throws std::exception. It must never crash, read out of bounds, or
// hang. (Run under a Debug build or a sanitizer to catch OOB that would not
// otherwise fault.)
void fuzz_decompress(unsigned iterations) {
    std::vector<std::vector<std::uint8_t>> seeds;
    seeds.push_back(axiom::compress(bytes_from_string(std::string(3000, 'a'))));
    {
        std::string s;
        for (int i = 0; i < 400; ++i) {
            s += "the quick brown fox jumps over ";
        }
        seeds.push_back(axiom::compress(bytes_from_string(s)));
    }
    {
        std::vector<std::uint8_t> v(4000);
        std::mt19937 r(7);
        std::generate(v.begin(), v.end(), [&] { return static_cast<std::uint8_t>(r()); });
        seeds.push_back(axiom::compress(v));
    }
    {
        axiom::CompressionOptions options;
        options.enable_optimal_parser = true;
        seeds.push_back(axiom::compress(bytes_from_string(std::string(2000, 'x') + "tail"), options));
    }
    // A small output cap keeps the fuzz itself bounded and exercises the
    // decompression-bomb guard on every iteration.
    constexpr std::size_t kFuzzOutputCap = std::size_t{8} << 20;  // 8 MiB
    axiom::DecompressionOptions options;
    options.max_output_size = kFuzzOutputCap;
    for (const auto& seed : seeds) {
        AXIOM_CHECK(!seed.empty());
        (void)axiom::decompress(seed, options);  // valid seeds must decode
    }

    std::mt19937 rng(0xF02Du);
    for (unsigned i = 0; i < iterations; ++i) {
        auto candidate = seeds[rng() % seeds.size()];
        mutate(candidate, rng);
        try {
            (void)axiom::decompress(candidate, options);
        } catch (const std::exception&) {
            // expected: malformed input is rejected, not crashed on
        }
    }
}

// Feed mutated archive containers to the directory parser and block decoder.
void fuzz_archive(unsigned iterations) {
    const auto root = make_temp_dir();
    const auto src = root / "s";
    fs::create_directories(src / "sub");
    write_all(src / "a.txt", bytes_from_string(std::string(800, 'q') + "data"));
    std::vector<std::uint8_t> bin(2500);
    for (std::size_t i = 0; i < bin.size(); ++i) {
        bin[i] = static_cast<std::uint8_t>((i * 11) & 0xFF);
    }
    write_all(src / "sub" / "b.bin", bin);

    const auto archive = root / "seed.axar";
    axiom::create_archive({src}, archive, {});
    const auto seed = read_all(archive);
    AXIOM_CHECK(!seed.empty());

    const auto bad = root / "fuzz.axar";
    std::mt19937 rng(0xA17Cu);
    for (unsigned i = 0; i < iterations; ++i) {
        auto candidate = seed;
        mutate(candidate, rng);
        write_all(bad, candidate);
        try {
            (void)axiom::list_archive(bad);
        } catch (const std::exception&) {
        }
        if (i % 3 == 0) {
            try {
                axiom::test_archive(bad);
            } catch (const std::exception&) {
            }
        }
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

}  // namespace

int main() {
    expect_roundtrip({});

    std::string repeated;
    for (int i = 0; i < 2000; ++i) {
        repeated += "function compressBlock(input, context) { return input + context; }\n";
    }
    expect_roundtrip(bytes_from_string(repeated));
    expect_huffman_roundtrip(bytes_from_string(repeated));
    expect_order1_roundtrip(bytes_from_string(repeated));
    expect_rans_roundtrip(bytes_from_string(repeated));
    expect_rans_order1_roundtrip(bytes_from_string(repeated));
    expect_optimal_lz77_roundtrip(bytes_from_string(repeated));
    expect_lazy_lz77_roundtrip(bytes_from_string(repeated));
    expect_tree_lz77_roundtrip(bytes_from_string(repeated));
    // Windows far smaller than the input force the cyclic buffer to wrap and
    // delete nodes repeatedly, covering the slid-window descent and splice paths.
    expect_tree_lz77_windowed_roundtrip(bytes_from_string(repeated), 256);
    expect_tree_lz77_windowed_roundtrip(bytes_from_string(repeated), 1024);
    expect_tree_lz77_windowed_roundtrip(bytes_from_string(repeated), 4099);  // non-power-of-two
    expect_split_stream_roundtrip(bytes_from_string(repeated));
    expect_sequence_stream_roundtrip(bytes_from_string(repeated));
    expect_parallel_block_roundtrip(bytes_from_string(repeated));
    expect_nested_task_executor();
    expect_fast_lz_roundtrip(bytes_from_string(repeated));

    std::vector<std::uint8_t> binary;
    for (int i = 0; i < 4096; ++i) {
        binary.push_back(static_cast<std::uint8_t>((i * 31) & 0xFF));
    }
    expect_roundtrip(binary);
    expect_huffman_roundtrip(binary);
    expect_order1_roundtrip(binary);
    expect_rans_roundtrip(binary);
    expect_rans_order1_roundtrip(binary);
    expect_optimal_lz77_roundtrip(binary);
    expect_tree_lz77_roundtrip(binary);
    expect_split_stream_roundtrip(binary);
    expect_slot_split_stream_roundtrip(binary);
    expect_sequence_stream_roundtrip(binary);
    expect_fast_lz_roundtrip(binary);

    // Interleave several recurring substrings so the parser sees a handful of
    // distinct distances that cycle, exercising rep0..rep3 selection and MTF.
    std::string rep_heavy;
    for (int i = 0; i < 1500; ++i) {
        rep_heavy += "alpha_token_one ";
        rep_heavy += "beta_token_two_is_longer ";
        if (i % 2 == 0) rep_heavy += "gamma_three ";
        if (i % 3 == 0) rep_heavy += "delta_four_variant ";
    }
    expect_roundtrip(bytes_from_string(rep_heavy));
    expect_split_stream_roundtrip(bytes_from_string(rep_heavy));
    expect_slot_split_stream_roundtrip(bytes_from_string(rep_heavy));
    expect_sequence_stream_roundtrip(bytes_from_string(rep_heavy));
    expect_fast_lz_roundtrip(bytes_from_string(rep_heavy));
    expect_optimal_lz77_roundtrip(bytes_from_string(rep_heavy));
    expect_tree_lz77_roundtrip(bytes_from_string(rep_heavy));

    std::mt19937 rng(0xA710CAFEu);
    std::vector<std::uint8_t> random(8192);
    std::generate(random.begin(), random.end(), [&] {
        return static_cast<std::uint8_t>(rng() & 0xFF);
    });
    expect_roundtrip(random);
    expect_order1_roundtrip(random);
    // A grossly truncated adaptive order-1 stream must be rejected: the bit
    // reader allows only the bounded final-flush overrun before throwing
    // instead of synthesizing zero bits until the declared size is reached.
    {
        const auto encoded = axiom::entropy::encode_order1(random);
        AXIOM_CHECK(encoded.has_value());
        const std::vector<std::uint8_t> truncated(
            encoded->begin(), encoded->begin() + static_cast<std::ptrdiff_t>(encoded->size() / 2));
        expect_throws([&] { (void)axiom::entropy::decode_order1(truncated, random.size()); });
    }
    expect_rans_roundtrip(random);
    expect_rans_order1_roundtrip(random);
    // Below the coder's minimum useful size it declines rather than encodes.
    AXIOM_CHECK(!axiom::entropy::encode_rans_order1(
        std::vector<std::uint8_t>(100, 0x41)).has_value());
    expect_tree_lz77_roundtrip(random);
    expect_tree_lz77_windowed_roundtrip(random, 512);
    expect_tree_lz77_windowed_roundtrip(bytes_from_string(rep_heavy), 300);
    expect_tree_lz77_windowed_roundtrip(binary, 128);
    expect_lazy_lz77_roundtrip(random);
    expect_lazy_lz77_roundtrip(binary);
    expect_lazy_lz77_roundtrip(bytes_from_string(rep_heavy));
    expect_fast_entropy_roundtrip(bytes_from_string(repeated));
    expect_fast_entropy_roundtrip(binary);
    expect_fast_entropy_roundtrip(random);
    expect_fast_entropy_roundtrip(bytes_from_string(rep_heavy));
    expect_fast_archive_roundtrip(bytes_from_string(repeated));
    expect_fast_archive_roundtrip(binary);
    expect_fast_archive_roundtrip(random);

    std::vector<std::uint8_t> long_distance;
    long_distance.reserve((96u << 10) * 2);
    for (std::size_t i = 0; i < (96u << 10); ++i) {
        long_distance.push_back(static_cast<std::uint8_t>('A' + (i % 23)));
    }
    long_distance.insert(long_distance.end(), long_distance.begin(), long_distance.end());
    expect_fast_lz_roundtrip(long_distance);

    auto archive = axiom::compress(bytes_from_string("checksum validation"));
    archive.back() ^= 0x55u;

    bool failed = false;
    try {
        (void)axiom::decompress(archive);
    } catch (const axiom::FormatError&) {
        failed = true;
    }
    AXIOM_CHECK(failed);

    test_compression_level_presets();
    test_reversible_transforms_and_v7();
    test_filtered_axar_roundtrip();
    test_crc32();
    test_blake3();
    test_reed_solomon();
    test_rans_edges();
    test_archive_roundtrip();
    test_archive_provider_layer();
    test_zip_provider_layer();
    test_safe_file_replacement();
#if defined(_WIN32)
    test_skip_unreadable_archive_inputs();
    test_unicode_path_archive_probe();
    test_unicode_archive_entry_names();
    test_iso_native_listing_provider_layer();
    test_system_archive_provider_layer();
    test_bundled_7z_provider_layer();
#endif
    test_zip_aes256_encryption();
    test_zip_aes_data_descriptor_compatibility();
    test_archive_symlinks();
    test_archive_hardlinks();
    test_archive_add();
    test_archive_file_manager_apis();
    test_archive_delete_repack();
    test_archive_update_sync();
    test_archive_comment_lock();
    test_archive_recovery_and_volumes();
    test_archive_authenticity_and_sfx();
    test_archive_encryption();
    test_archive_encrypted_directory();
    test_archive_operation_control();
    test_compression_estimator();
#if defined(_WIN32)
    test_windows_metadata();
    test_archive_ads();
#endif
    test_archive_safety();
    test_extract_link_safety();
    test_decompress_bomb();
    test_split_stream_size_bomb();
    test_sequence_stream_truncation();
    fuzz_decompress(20000);
    fuzz_archive(1500);

    std::cout << "all tests passed (codec + archive + safety + fuzz)\n";
    return 0;
}
