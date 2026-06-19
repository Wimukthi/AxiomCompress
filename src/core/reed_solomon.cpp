#include "core/reed_solomon.hpp"

#include <array>
#include <stdexcept>

namespace axiom::core {
namespace {

// GF(2^8) with primitive polynomial 0x11D (x^8 + x^4 + x^3 + x^2 + 1).
struct GaloisField {
    std::array<std::uint8_t, 512> exp{};
    std::array<std::uint8_t, 256> log{};

    GaloisField() {
        int x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(x);
            log[static_cast<std::size_t>(x)] = static_cast<std::uint8_t>(i);
            x <<= 1;
            if (x & 0x100) {
                x ^= 0x11D;
            }
        }
        // Duplicate so exp[a+b] needs no modulo for a,b in [0,255).
        for (int i = 255; i < 512; ++i) {
            exp[static_cast<std::size_t>(i)] = exp[static_cast<std::size_t>(i - 255)];
        }
    }
};

const GaloisField& gf() {
    static const GaloisField field;
    return field;
}

std::uint8_t gf_mul(std::uint8_t a, std::uint8_t b) {
    if (a == 0 || b == 0) {
        return 0;
    }
    const auto& f = gf();
    return f.exp[static_cast<std::size_t>(f.log[a]) + f.log[b]];
}

std::uint8_t gf_div(std::uint8_t a, std::uint8_t b) {
    if (a == 0) {
        return 0;
    }
    const auto& f = gf();
    return f.exp[static_cast<std::size_t>(f.log[a]) + 255 - f.log[b]];
}

std::uint8_t gf_pow(std::uint8_t base, int exponent) {
    std::uint8_t result = 1;
    for (int i = 0; i < exponent; ++i) {
        result = gf_mul(result, base);
    }
    return result;
}

// A dense matrix over GF(2^8), row-major.
struct Matrix {
    int rows = 0;
    int cols = 0;
    std::vector<std::uint8_t> data;

    Matrix() = default;
    Matrix(int r, int c) : rows(r), cols(c), data(static_cast<std::size_t>(r) * c, 0) {}

    std::uint8_t& at(int r, int c) { return data[static_cast<std::size_t>(r) * cols + c]; }
    std::uint8_t at(int r, int c) const { return data[static_cast<std::size_t>(r) * cols + c]; }

    Matrix multiply(const Matrix& rhs) const {
        Matrix out(rows, rhs.cols);
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < rhs.cols; ++c) {
                std::uint8_t acc = 0;
                for (int k = 0; k < cols; ++k) {
                    acc ^= gf_mul(at(r, k), rhs.at(k, c));
                }
                out.at(r, c) = acc;
            }
        }
        return out;
    }

    // Gauss-Jordan inverse of a square matrix. Throws if singular.
    Matrix inverse() const {
        Matrix work(*this);
        Matrix inv(rows, rows);
        for (int i = 0; i < rows; ++i) {
            inv.at(i, i) = 1;
        }
        for (int col = 0; col < rows; ++col) {
            if (work.at(col, col) == 0) {
                int swap = -1;
                for (int r = col + 1; r < rows; ++r) {
                    if (work.at(r, col) != 0) {
                        swap = r;
                        break;
                    }
                }
                if (swap < 0) {
                    throw std::runtime_error("Reed-Solomon matrix is singular");
                }
                for (int c = 0; c < rows; ++c) {
                    std::swap(work.at(col, c), work.at(swap, c));
                    std::swap(inv.at(col, c), inv.at(swap, c));
                }
            }
            const std::uint8_t pivot = work.at(col, col);
            for (int c = 0; c < rows; ++c) {
                work.at(col, c) = gf_div(work.at(col, c), pivot);
                inv.at(col, c) = gf_div(inv.at(col, c), pivot);
            }
            for (int r = 0; r < rows; ++r) {
                if (r == col) {
                    continue;
                }
                const std::uint8_t factor = work.at(r, col);
                if (factor == 0) {
                    continue;
                }
                for (int c = 0; c < rows; ++c) {
                    work.at(r, c) ^= gf_mul(factor, work.at(col, c));
                    inv.at(r, c) ^= gf_mul(factor, inv.at(col, c));
                }
            }
        }
        return inv;
    }
};

// Build the systematic encoding matrix: a (data+parity) × data matrix whose top
// data × data block is the identity, so the first `data` shards pass through and the
// remaining rows produce the parity. Derived from a Vandermonde matrix made
// systematic by multiplying through the inverse of its top square block.
Matrix build_encoding_matrix(int data, int parity) {
    const int total = data + parity;
    Matrix vander(total, data);
    for (int r = 0; r < total; ++r) {
        for (int c = 0; c < data; ++c) {
            vander.at(r, c) = gf_pow(static_cast<std::uint8_t>(r + 1), c);
        }
    }
    Matrix top(data, data);
    for (int r = 0; r < data; ++r) {
        for (int c = 0; c < data; ++c) {
            top.at(r, c) = vander.at(r, c);
        }
    }
    return vander.multiply(top.inverse());
}

}  // namespace

ReedSolomon::ReedSolomon(int data_shards, int parity_shards)
    : data_shards_(data_shards), parity_shards_(parity_shards) {
    if (data_shards < 1 || parity_shards < 1 || data_shards + parity_shards > 255) {
        throw std::invalid_argument("Reed-Solomon shard counts out of range");
    }
    const Matrix matrix = build_encoding_matrix(data_shards, parity_shards);
    matrix_ = matrix.data;
}

void ReedSolomon::encode(const std::vector<std::span<const std::uint8_t>>& data,
                         const std::vector<std::span<std::uint8_t>>& parity) const {
    if (static_cast<int>(data.size()) != data_shards_ ||
        static_cast<int>(parity.size()) != parity_shards_) {
        throw std::invalid_argument("Reed-Solomon encode: wrong shard count");
    }
    const std::size_t len = data.empty() ? 0 : data[0].size();
    for (int p = 0; p < parity_shards_; ++p) {
        const int row = data_shards_ + p;
        for (std::size_t byte = 0; byte < len; ++byte) {
            std::uint8_t acc = 0;
            for (int d = 0; d < data_shards_; ++d) {
                const std::uint8_t coeff = matrix_[static_cast<std::size_t>(row) * data_shards_ + d];
                acc ^= gf_mul(coeff, data[static_cast<std::size_t>(d)][byte]);
            }
            parity[static_cast<std::size_t>(p)][byte] = acc;
        }
    }
}

bool ReedSolomon::reconstruct(std::vector<std::vector<std::uint8_t>>& shards,
                              const std::vector<bool>& present) const {
    const int total = total_shards();
    if (static_cast<int>(shards.size()) != total || static_cast<int>(present.size()) != total) {
        throw std::invalid_argument("Reed-Solomon reconstruct: wrong shard count");
    }
    int have = 0;
    std::size_t len = 0;
    for (int i = 0; i < total; ++i) {
        if (present[static_cast<std::size_t>(i)]) {
            ++have;
            len = shards[static_cast<std::size_t>(i)].size();
        }
    }
    if (have < data_shards_) {
        return false;  // too few shards survive
    }
    if (have == total) {
        return true;  // nothing to do
    }

    // Pick any `data_shards` present rows of the encoding matrix; the present shards
    // equal (subMatrix * original_data), so original_data = subMatrix^-1 * present.
    Matrix sub(data_shards_, data_shards_);
    std::vector<int> source_rows;
    source_rows.reserve(static_cast<std::size_t>(data_shards_));
    for (int i = 0; i < total && static_cast<int>(source_rows.size()) < data_shards_; ++i) {
        if (present[static_cast<std::size_t>(i)]) {
            source_rows.push_back(i);
        }
    }
    for (int r = 0; r < data_shards_; ++r) {
        for (int c = 0; c < data_shards_; ++c) {
            sub.at(r, c) =
                matrix_[static_cast<std::size_t>(source_rows[static_cast<std::size_t>(r)]) *
                            data_shards_ +
                        c];
        }
    }
    const Matrix sub_inv = sub.inverse();

    // Recover every missing data shard as a linear combination of the present shards.
    for (int d = 0; d < data_shards_; ++d) {
        if (present[static_cast<std::size_t>(d)]) {
            continue;
        }
        std::vector<std::uint8_t> recovered(len, 0);
        for (int j = 0; j < data_shards_; ++j) {
            const std::uint8_t coeff = sub_inv.at(d, j);
            if (coeff == 0) {
                continue;
            }
            const auto& src = shards[static_cast<std::size_t>(source_rows[static_cast<std::size_t>(j)])];
            for (std::size_t byte = 0; byte < len; ++byte) {
                recovered[byte] ^= gf_mul(coeff, src[byte]);
            }
        }
        shards[static_cast<std::size_t>(d)] = std::move(recovered);
    }

    // Recompute any missing parity shards from the now-complete data shards.
    for (int p = 0; p < parity_shards_; ++p) {
        const int idx = data_shards_ + p;
        if (present[static_cast<std::size_t>(idx)]) {
            continue;
        }
        std::vector<std::uint8_t> recovered(len, 0);
        const int row = data_shards_ + p;
        for (int dpos = 0; dpos < data_shards_; ++dpos) {
            const std::uint8_t coeff = matrix_[static_cast<std::size_t>(row) * data_shards_ + dpos];
            if (coeff == 0) {
                continue;
            }
            const auto& src = shards[static_cast<std::size_t>(dpos)];
            for (std::size_t byte = 0; byte < len; ++byte) {
                recovered[byte] ^= gf_mul(coeff, src[byte]);
            }
        }
        shards[static_cast<std::size_t>(idx)] = std::move(recovered);
    }
    return true;
}

}  // namespace axiom::core
