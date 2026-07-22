#include "core/benchmark_corpus.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace axiom::core {
namespace {

constexpr std::size_t kProgressInterval = 1u << 20;
constexpr std::size_t kLiteralPrefix = 1u << 10;

class SyntheticRandom {
public:
    explicit SyntheticRandom(std::uint64_t seed) : state_(seed) {}

    std::uint64_t next64() {
        // SplitMix64 is specified entirely in unsigned arithmetic, making the
        // generated corpus identical across compilers and processor families.
        std::uint64_t value = (state_ += 0x9E37'79B9'7F4A'7C15ull);
        value = (value ^ (value >> 30)) * 0xBF58'476D'1CE4'E5B9ull;
        value = (value ^ (value >> 27)) * 0x94D0'49BB'1331'11EBull;
        return value ^ (value >> 31);
    }

    std::uint32_t next32() {
        return static_cast<std::uint32_t>(next64() >> 32);
    }

private:
    std::uint64_t state_;
};

class ProgressReporter {
public:
    explicit ProgressReporter(const BenchmarkCorpusProgress& callback)
        : callback_(callback) {}

    void update(std::size_t completed, bool force = false) {
        if (!callback_) return;
        if (force || completed >= next_) {
            callback_(completed);
            next_ = completed > std::numeric_limits<std::size_t>::max() - kProgressInterval
                ? std::numeric_limits<std::size_t>::max()
                : completed + kProgressInterval;
        }
    }

private:
    const BenchmarkCorpusProgress& callback_;
    std::size_t next_ = kProgressInterval;
};

unsigned take_bits(std::uint32_t& value, unsigned count) {
    const std::uint32_t mask = count == 32
        ? std::numeric_limits<std::uint32_t>::max()
        : (std::uint32_t{1} << count) - 1;
    const unsigned result = value & mask;
    value >>= count;
    return result;
}

unsigned short_match_component(std::uint32_t& value) {
    const unsigned width = 1 + take_bits(value, 2);
    return take_bits(value, width);
}

std::size_t choose_log_distance(SyntheticRandom& random, std::size_t history,
                                std::size_t window_size) {
    const std::size_t limit = std::min(history, std::max<std::size_t>(1, window_size));
    const unsigned max_bits = std::bit_width(limit);
    const unsigned min_bits = std::min(6u, max_bits);
    const unsigned width = min_bits +
        (max_bits == min_bits ? 0u : random.next32() % (max_bits - min_bits + 1));
    const std::uint64_t mask = width >= 64
        ? std::numeric_limits<std::uint64_t>::max()
        : (std::uint64_t{1} << width) - 1;
    return 1 + static_cast<std::size_t>(random.next64() & mask) % limit;
}

void fill_lz_segment(ByteVector& output, std::size_t begin, std::size_t end,
                     std::size_t window_size, std::uint64_t seed,
                     ProgressReporter& progress) {
    SyntheticRandom random(seed);
    std::size_t position = begin;
    std::size_t previous_distance = 1;

    const std::size_t literal_end = std::min(end, begin + kLiteralPrefix);
    while (position < literal_end) {
        output[position++] = static_cast<std::uint8_t>(random.next32());
    }

    while (position < end) {
        std::uint32_t bits = random.next32();
        const std::size_t history = position - begin;
        if ((take_bits(bits, 1) == 0) || history == 0) {
            output[position++] = static_cast<std::uint8_t>(bits);
        } else {
            std::size_t length = 1 + short_match_component(bits);
            const bool reuse_distance = take_bits(bits, 3) == 0 &&
                previous_distance <= std::min(history, window_size);
            if (!reuse_distance) {
                length += short_match_component(bits);
                previous_distance = choose_log_distance(
                    random, history, window_size);
            }
            length = std::min(length, end - position);
            for (std::size_t copied = 0; copied < length; ++copied) {
                output[position] = output[position - previous_distance];
                ++position;
            }
        }
        progress.update(position);
    }
}

void fill_lz(ByteVector& output, const BenchmarkCorpusOptions& options,
             ProgressReporter& progress) {
    const std::size_t window = std::max<std::size_t>(1, options.window_size);
    const std::size_t segment = options.segment_size == 0
        ? output.size()
        : std::max<std::size_t>(kLiteralPrefix, options.segment_size);
    std::size_t begin = 0;
    std::uint64_t segment_index = 0;
    while (begin < output.size()) {
        const std::size_t end = begin + std::min(segment, output.size() - begin);
        fill_lz_segment(output, begin, end, window,
                        options.seed + segment_index * 0xD1B5'4A32'D192'ED03ull,
                        progress);
        begin = end;
        ++segment_index;
    }
}

void fill_structured_text(ByteVector& output, const BenchmarkCorpusOptions& options,
                          ProgressReporter& progress) {
    SyntheticRandom random(options.seed);
    static constexpr std::array<const char*, 5> levels{
        "TRACE", "DEBUG", "INFO", "WARN", "ERROR"};
    static constexpr std::array<const char*, 6> extensions{
        "cpp", "json", "log", "txt", "dll", "dat"};
    static constexpr std::array<const char*, 6> messages{
        "operation completed", "cache entry refreshed", "worker accepted task",
        "input stream opened", "metadata record updated", "retry scheduled"};

    std::size_t position = 0;
    std::uint64_t record = 0;
    while (position < output.size()) {
        const std::uint32_t value = random.next32();
        char line[320]{};
        const int length = std::snprintf(
            line, sizeof(line),
            "2026-07-%02uT%02u:%02u:%02u.%03uZ [%s] worker=%02u "
            "path=C:\\Data\\project_%03u\\module_%02u\\file_%05u.%s "
            "bytes=%u duration_us=%u message=\"%s\" request=%08llx\r\n",
            1u + static_cast<unsigned>(record % 28),
            static_cast<unsigned>((record / 17) % 24),
            static_cast<unsigned>((record / 7) % 60),
            static_cast<unsigned>(record % 60), value % 1000,
            levels[(value >> 3) % levels.size()], (value >> 8) % 64,
            (value >> 12) % 200, (value >> 19) % 32,
            static_cast<unsigned>((record * 13 + (value & 31)) % 100000),
            extensions[(value >> 24) % extensions.size()],
            4096u + value % (16u << 20), 20u + (value >> 5) % 900000,
            messages[(value >> 16) % messages.size()],
            static_cast<unsigned long long>(random.next64()));
        if (length <= 0) {
            throw std::runtime_error("failed to generate benchmark text corpus");
        }
        const std::size_t count = std::min(
            output.size() - position,
            std::min<std::size_t>(static_cast<std::size_t>(length), sizeof(line) - 1));
        std::memcpy(output.data() + position, line, count);
        position += count;
        ++record;
        progress.update(position);
    }
}

void fill_random(ByteVector& output, const BenchmarkCorpusOptions& options,
                 ProgressReporter& progress) {
    SyntheticRandom random(options.seed);
    std::size_t position = 0;
    while (position + sizeof(std::uint64_t) <= output.size()) {
        const std::uint64_t value = random.next64();
        for (unsigned byte = 0; byte < 8; ++byte) {
            output[position + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
        }
        position += 8;
        progress.update(position);
    }
    if (position < output.size()) {
        std::uint64_t value = random.next64();
        while (position < output.size()) {
            output[position++] = static_cast<std::uint8_t>(value);
            value >>= 8;
        }
    }
}

}  // namespace

ByteVector generate_benchmark_corpus(const BenchmarkCorpusOptions& options,
                                     const BenchmarkCorpusProgress& progress_callback) {
    ByteVector output(options.size);
    ProgressReporter progress(progress_callback);
    if (output.empty()) {
        progress.update(0, true);
        return output;
    }

    switch (options.kind) {
        case BenchmarkCorpusKind::lz_synthetic:
            fill_lz(output, options, progress);
            break;
        case BenchmarkCorpusKind::structured_text:
            fill_structured_text(output, options, progress);
            break;
        case BenchmarkCorpusKind::random:
            fill_random(output, options, progress);
            break;
    }
    progress.update(output.size(), true);
    return output;
}

}  // namespace axiom::core
