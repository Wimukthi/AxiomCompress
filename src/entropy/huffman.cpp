#include "entropy/huffman.hpp"

#include "codec/varint.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <queue>
#include <vector>

namespace axiom::entropy {
namespace {

constexpr std::size_t kSymbolCount = 256;
constexpr std::uint8_t kMaxCodeLength = 32;
constexpr std::size_t kCodeLengthTableBytes = kSymbolCount;

struct TreeNode {
    std::uint64_t frequency = 0;
    int symbol = -1;
    int left = -1;
    int right = -1;
};

struct QueueEntry {
    std::uint64_t frequency = 0;
    int node_index = -1;

    bool operator>(const QueueEntry& other) const {
        if (frequency != other.frequency) {
            return frequency > other.frequency;
        }

        return node_index > other.node_index;
    }
};

struct Code {
    std::uint64_t bits = 0;
    std::uint8_t length = 0;
};

struct DecodeNode {
    int child[2] = {-1, -1};
    int symbol = -1;
};

class BitWriter {
public:
    void write(std::uint64_t bits, std::uint8_t length) {
        for (std::uint8_t remaining = length; remaining > 0; --remaining) {
            const auto bit = static_cast<std::uint8_t>((bits >> (remaining - 1)) & 1u);
            current_ = static_cast<std::uint8_t>((current_ << 1) | bit);
            ++filled_;

            if (filled_ == 8) {
                bytes_.push_back(current_);
                current_ = 0;
                filled_ = 0;
            }
        }
    }

    ByteVector finish() {
        if (filled_ != 0) {
            bytes_.push_back(static_cast<std::uint8_t>(current_ << (8 - filled_)));
        }

        return std::move(bytes_);
    }

private:
    ByteVector bytes_;
    std::uint8_t current_ = 0;
    std::uint8_t filled_ = 0;
};

class BitReader {
public:
    explicit BitReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    int read_bit() {
        if (cursor_ >= bytes_.size()) {
            throw FormatError("truncated Huffman bitstream");
        }

        const auto bit = (bytes_[cursor_] >> (7 - bit_index_)) & 1u;
        ++bit_index_;
        if (bit_index_ == 8) {
            bit_index_ = 0;
            ++cursor_;
        }

        return static_cast<int>(bit);
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t cursor_ = 0;
    std::uint8_t bit_index_ = 0;
};

std::array<std::uint64_t, kSymbolCount> count_frequencies(std::span<const std::uint8_t> input) {
    std::array<std::uint64_t, kSymbolCount> frequencies{};

    for (const auto byte : input) {
        ++frequencies[byte];
    }

    return frequencies;
}

void fill_code_lengths(const std::vector<TreeNode>& nodes,
                       int node_index,
                       std::uint8_t depth,
                       std::array<std::uint8_t, kSymbolCount>& lengths) {
    const auto& node = nodes[static_cast<std::size_t>(node_index)];

    if (node.symbol >= 0) {
        lengths[static_cast<std::size_t>(node.symbol)] = std::max<std::uint8_t>(1, depth);
        return;
    }

    fill_code_lengths(nodes, node.left, static_cast<std::uint8_t>(depth + 1), lengths);
    fill_code_lengths(nodes, node.right, static_cast<std::uint8_t>(depth + 1), lengths);
}

std::optional<std::array<std::uint8_t, kSymbolCount>> build_code_lengths(
    const std::array<std::uint64_t, kSymbolCount>& frequencies) {
    std::vector<TreeNode> nodes;
    std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<>> queue;

    for (std::size_t symbol = 0; symbol < frequencies.size(); ++symbol) {
        if (frequencies[symbol] == 0) {
            continue;
        }

        const auto node_index = static_cast<int>(nodes.size());
        nodes.push_back(TreeNode{frequencies[symbol], static_cast<int>(symbol), -1, -1});
        queue.push(QueueEntry{frequencies[symbol], node_index});
    }

    if (queue.empty()) {
        return std::nullopt;
    }

    if (queue.size() == 1) {
        std::array<std::uint8_t, kSymbolCount> lengths{};
        lengths[static_cast<std::size_t>(nodes[static_cast<std::size_t>(queue.top().node_index)].symbol)] = 1;
        return lengths;
    }

    while (queue.size() > 1) {
        const auto left = queue.top();
        queue.pop();
        const auto right = queue.top();
        queue.pop();

        const auto node_index = static_cast<int>(nodes.size());
        nodes.push_back(TreeNode{
            left.frequency + right.frequency,
            -1,
            left.node_index,
            right.node_index,
        });
        queue.push(QueueEntry{left.frequency + right.frequency, node_index});
    }

    std::array<std::uint8_t, kSymbolCount> lengths{};
    fill_code_lengths(nodes, queue.top().node_index, 0, lengths);

    if (*std::max_element(lengths.begin(), lengths.end()) > kMaxCodeLength) {
        return std::nullopt;
    }

    return lengths;
}

std::array<Code, kSymbolCount> build_canonical_codes(
    const std::array<std::uint8_t, kSymbolCount>& lengths) {
    std::array<std::uint32_t, kMaxCodeLength + 1> length_counts{};
    for (const auto length : lengths) {
        if (length > kMaxCodeLength) {
            throw FormatError("Huffman code length exceeds decoder limit");
        }

        if (length != 0) {
            ++length_counts[length];
        }
    }

    std::array<std::uint64_t, kMaxCodeLength + 1> next_code{};
    std::uint64_t code = 0;

    for (std::size_t bits = 1; bits <= kMaxCodeLength; ++bits) {
        code = (code + length_counts[bits - 1]) << 1;
        next_code[bits] = code;
    }

    std::array<Code, kSymbolCount> codes{};
    for (std::size_t symbol = 0; symbol < kSymbolCount; ++symbol) {
        const auto length = lengths[symbol];
        if (length == 0) {
            continue;
        }

        const auto limit = 1ULL << length;
        if (next_code[length] >= limit) {
            throw FormatError("oversubscribed Huffman table");
        }

        codes[symbol] = Code{next_code[length]++, length};
    }

    return codes;
}

std::vector<DecodeNode> build_decode_tree(const std::array<Code, kSymbolCount>& codes) {
    std::vector<DecodeNode> tree(1);

    for (std::size_t symbol = 0; symbol < codes.size(); ++symbol) {
        const auto code = codes[symbol];
        if (code.length == 0) {
            continue;
        }

        int node_index = 0;
        for (std::uint8_t remaining = code.length; remaining > 0; --remaining) {
            const auto bit = static_cast<int>((code.bits >> (remaining - 1)) & 1u);
            auto next = tree[static_cast<std::size_t>(node_index)].child[bit];

            if (next == -1) {
                next = static_cast<int>(tree.size());
                tree[static_cast<std::size_t>(node_index)].child[bit] = next;
                tree.push_back(DecodeNode{});
            }

            node_index = next;
        }

        auto& leaf = tree[static_cast<std::size_t>(node_index)];
        if (leaf.symbol != -1) {
            throw FormatError("duplicate Huffman code");
        }

        leaf.symbol = static_cast<int>(symbol);
    }

    return tree;
}

std::size_t single_symbol(const std::array<std::uint8_t, kSymbolCount>& lengths) {
    std::size_t found = kSymbolCount;

    for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
        if (lengths[symbol] == 0) {
            continue;
        }

        if (found != kSymbolCount) {
            return kSymbolCount;
        }

        found = symbol;
    }

    return found;
}

}  // namespace

std::optional<ByteVector> encode_huffman(std::span<const std::uint8_t> input) {
    if (input.empty()) {
        return std::nullopt;
    }

    const auto frequencies = count_frequencies(input);
    const auto maybe_lengths = build_code_lengths(frequencies);
    if (!maybe_lengths) {
        return std::nullopt;
    }

    const auto& lengths = *maybe_lengths;
    const auto codes = build_canonical_codes(lengths);

    ByteVector output;
    codec::write_varuint(output, input.size());
    output.insert(output.end(), lengths.begin(), lengths.end());

    if (single_symbol(lengths) != kSymbolCount) {
        return output;
    }

    BitWriter writer;
    for (const auto byte : input) {
        const auto code = codes[byte];
        writer.write(code.bits, code.length);
    }

    auto bits = writer.finish();
    output.insert(output.end(), bits.begin(), bits.end());
    return output;
}

ByteVector decode_huffman(std::span<const std::uint8_t> encoded,
                          std::size_t max_output_size) {
    std::size_t cursor = 0;
    const auto decoded_size = static_cast<std::size_t>(codec::read_varuint(encoded, cursor));

    if (decoded_size > max_output_size) {
        throw FormatError("Huffman output exceeds block limit");
    }

    if (encoded.size() - cursor < kCodeLengthTableBytes) {
        throw FormatError("truncated Huffman code-length table");
    }

    std::array<std::uint8_t, kSymbolCount> lengths{};
    std::copy_n(encoded.begin() + static_cast<std::ptrdiff_t>(cursor),
                lengths.size(),
                lengths.begin());
    cursor += lengths.size();

    for (const auto length : lengths) {
        if (length > kMaxCodeLength) {
            throw FormatError("Huffman code length exceeds decoder limit");
        }
    }

    if (decoded_size == 0) {
        return {};
    }

    const auto only_symbol = single_symbol(lengths);
    if (only_symbol != kSymbolCount) {
        if (cursor != encoded.size()) {
            throw FormatError("single-symbol Huffman stream has trailing bits");
        }

        return ByteVector(decoded_size, static_cast<std::uint8_t>(only_symbol));
    }

    const auto codes = build_canonical_codes(lengths);
    const auto tree = build_decode_tree(codes);

    ByteVector output;
    codec::bounded_reserve(output, decoded_size);
    BitReader reader(encoded.subspan(cursor));

    while (output.size() < decoded_size) {
        int node_index = 0;

        while (tree[static_cast<std::size_t>(node_index)].symbol == -1) {
            const auto bit = reader.read_bit();
            node_index = tree[static_cast<std::size_t>(node_index)].child[bit];

            if (node_index == -1) {
                throw FormatError("invalid Huffman code");
            }
        }

        output.push_back(static_cast<std::uint8_t>(
            tree[static_cast<std::size_t>(node_index)].symbol));
    }

    return output;
}

}  // namespace axiom::entropy
