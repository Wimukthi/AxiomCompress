#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace axiom::codec {

inline std::uint32_t sample_load_u32(std::span<const std::uint8_t> input,
                                     std::size_t position) {
    std::uint32_t value = 0;
    std::memcpy(&value, input.data() + position, sizeof(value));
    if constexpr (std::endian::native == std::endian::big) {
        value = ((value & 0x000000FFu) << 24) |
                ((value & 0x0000FF00u) << 8) |
                ((value & 0x00FF0000u) >> 8) |
                ((value & 0xFF000000u) >> 24);
    }
    return value;
}

inline std::uint32_t avalanche32(std::uint32_t value) {
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

// Conservative fast-store gate. It only returns true when a sampled byte stream
// has near-random byte entropy and very few repeated four-byte patterns. That
// skips expensive match finding on data that would end up stored anyway while
// avoiding high-entropy but trivially repetitive streams such as 0..255 cycles.
inline bool likely_incompressible(std::span<const std::uint8_t> input) {
    constexpr std::size_t kMinInput = std::size_t{1} << 20;
    constexpr std::size_t kMaxEntropySample = std::size_t{256} << 10;
    constexpr std::size_t kMaxHashSamples = 8192;
    constexpr double kEntropyThreshold = 7.92;
    constexpr double kDuplicateThreshold = 0.003;

    if (input.size() < kMinInput) {
        return false;
    }

    const std::size_t entropy_samples = std::min(kMaxEntropySample, input.size());
    const std::size_t entropy_step = std::max<std::size_t>(1, input.size() / entropy_samples);
    std::array<std::uint32_t, 256> histogram{};
    std::size_t sampled = 0;
    for (std::size_t position = 0; position < input.size() && sampled < entropy_samples;
         position += entropy_step, ++sampled) {
        ++histogram[input[position]];
    }
    if (sampled == 0) {
        return false;
    }

    const auto total = static_cast<double>(sampled);
    double entropy = 0.0;
    for (const auto count : histogram) {
        if (count == 0) {
            continue;
        }
        const double p = static_cast<double>(count) / total;
        entropy -= p * std::log2(p);
    }
    if (entropy < kEntropyThreshold) {
        return false;
    }

    const std::size_t hash_samples = std::min(kMaxHashSamples, input.size() - 3);
    const std::size_t hash_span = input.size() - 4;
    const std::size_t hash_step = std::max<std::size_t>(1, hash_span / hash_samples);
    std::vector<std::uint32_t> hashes;
    hashes.reserve(hash_samples);
    for (std::size_t position = 0; position <= hash_span && hashes.size() < hash_samples;
         position += hash_step) {
        hashes.push_back(avalanche32(sample_load_u32(input, position)));
    }
    if (hashes.size() < 1024) {
        return false;
    }

    std::sort(hashes.begin(), hashes.end());
    std::size_t duplicates = 0;
    for (std::size_t i = 1; i < hashes.size(); ++i) {
        duplicates += hashes[i] == hashes[i - 1] ? 1 : 0;
    }
    const double duplicate_rate = static_cast<double>(duplicates) /
                                  static_cast<double>(hashes.size());
    return duplicate_rate < kDuplicateThreshold;
}

}  // namespace axiom::codec
