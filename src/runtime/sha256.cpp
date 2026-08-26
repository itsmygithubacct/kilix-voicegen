#include "runtime/sha256.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>

namespace kgv {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

constexpr std::uint32_t rotate_right(std::uint32_t value,
                                     unsigned int count) noexcept {
    return (value >> count) | (value << (32U - count));
}

constexpr std::uint32_t choose(std::uint32_t x, std::uint32_t y,
                               std::uint32_t z) noexcept {
    return (x & y) ^ (~x & z);
}

constexpr std::uint32_t majority(std::uint32_t x, std::uint32_t y,
                                 std::uint32_t z) noexcept {
    return (x & y) ^ (x & z) ^ (y & z);
}

constexpr std::uint32_t big_sigma0(std::uint32_t x) noexcept {
    return rotate_right(x, 2U) ^ rotate_right(x, 13U) ^ rotate_right(x, 22U);
}

constexpr std::uint32_t big_sigma1(std::uint32_t x) noexcept {
    return rotate_right(x, 6U) ^ rotate_right(x, 11U) ^ rotate_right(x, 25U);
}

constexpr std::uint32_t small_sigma0(std::uint32_t x) noexcept {
    return rotate_right(x, 7U) ^ rotate_right(x, 18U) ^ (x >> 3U);
}

constexpr std::uint32_t small_sigma1(std::uint32_t x) noexcept {
    return rotate_right(x, 17U) ^ rotate_right(x, 19U) ^ (x >> 10U);
}

}  // namespace

Sha256::Sha256()
    : state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
             0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U} {}

void Sha256::transform(const std::uint8_t block[64]) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t i = 0; i < 16; ++i) {
        const std::size_t offset = i * 4U;
        words[i] = (static_cast<std::uint32_t>(block[offset]) << 24U) |
                   (static_cast<std::uint32_t>(block[offset + 1U]) << 16U) |
                   (static_cast<std::uint32_t>(block[offset + 2U]) << 8U) |
                   static_cast<std::uint32_t>(block[offset + 3U]);
    }
    for (std::size_t i = 16; i < words.size(); ++i) {
        words[i] = small_sigma1(words[i - 2U]) + words[i - 7U] +
                   small_sigma0(words[i - 15U]) + words[i - 16U];
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (std::size_t i = 0; i < words.size(); ++i) {
        const std::uint32_t first = h + big_sigma1(e) + choose(e, f, g) +
                                    kRoundConstants[i] + words[i];
        const std::uint32_t second = big_sigma0(a) + majority(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(const void *data, std::size_t size) {
    if (size == 0U) {
        return;
    }
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    total_bytes_ += static_cast<std::uint64_t>(size);

    while (size > 0U) {
        const std::size_t available = buffer_.size() - buffered_;
        const std::size_t amount = std::min(size, available);
        std::copy_n(bytes, amount, buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_));
        buffered_ += amount;
        bytes += amount;
        size -= amount;
        if (buffered_ == buffer_.size()) {
            transform(buffer_.data());
            buffered_ = 0U;
        }
    }
}

Sha256Digest Sha256::finish() const {
    Sha256 final = *this;
    const std::uint64_t bit_count = final.total_bytes_ * UINT64_C(8);

    final.buffer_[final.buffered_++] = 0x80U;
    if (final.buffered_ > 56U) {
        std::fill(final.buffer_.begin() + static_cast<std::ptrdiff_t>(final.buffered_),
                  final.buffer_.end(), 0U);
        final.transform(final.buffer_.data());
        final.buffered_ = 0U;
    }
    std::fill(final.buffer_.begin() + static_cast<std::ptrdiff_t>(final.buffered_),
              final.buffer_.begin() + 56, 0U);
    for (std::size_t i = 0; i < 8U; ++i) {
        const unsigned int shift = static_cast<unsigned int>((7U - i) * 8U);
        final.buffer_[56U + i] = static_cast<std::uint8_t>(bit_count >> shift);
    }
    final.transform(final.buffer_.data());

    Sha256Digest digest{};
    for (std::size_t i = 0; i < final.state_.size(); ++i) {
        const std::uint32_t word = final.state_[i];
        digest[i * 4U] = static_cast<std::uint8_t>(word >> 24U);
        digest[i * 4U + 1U] = static_cast<std::uint8_t>(word >> 16U);
        digest[i * 4U + 2U] = static_cast<std::uint8_t>(word >> 8U);
        digest[i * 4U + 3U] = static_cast<std::uint8_t>(word);
    }
    return digest;
}

Sha256Digest sha256_bytes(std::string_view bytes) {
    Sha256 hash;
    hash.update(bytes.data(), bytes.size());
    return hash.finish();
}

std::string sha256_hex(const Sha256Digest &digest) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.resize(digest.size() * 2U);
    for (std::size_t i = 0; i < digest.size(); ++i) {
        result[i * 2U] = kHex[digest[i] >> 4U];
        result[i * 2U + 1U] = kHex[digest[i] & 0x0fU];
    }
    return result;
}

std::string sha256_hex(std::string_view bytes) {
    return sha256_hex(sha256_bytes(bytes));
}

bool sha256_file(const std::filesystem::path &path,
                 Sha256Digest *digest,
                 std::uint64_t *byte_count,
                 std::string *error) {
    if (digest == nullptr || byte_count == nullptr || error == nullptr) {
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        *error = "could not open a required model file";
        return false;
    }

    Sha256 hash;
    std::array<char, 32768> buffer{};
    std::uint64_t total = 0U;
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = stream.gcount();
        if (count > 0) {
            const auto amount = static_cast<std::size_t>(count);
            if (total > std::numeric_limits<std::uint64_t>::max() -
                            static_cast<std::uint64_t>(amount)) {
                *error = "required model file is too large";
                return false;
            }
            hash.update(buffer.data(), amount);
            total += static_cast<std::uint64_t>(amount);
        }
    }
    if (!stream.eof()) {
        *error = "could not read a required model file";
        return false;
    }
    *digest = hash.finish();
    *byte_count = total;
    error->clear();
    return true;
}

bool is_lower_sha256(std::string_view value) {
    if (value.size() != 64U) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
    });
}

}  // namespace kgv
