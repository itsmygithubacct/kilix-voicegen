#ifndef KGV_RUNTIME_SHA256_H
#define KGV_RUNTIME_SHA256_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace kgv {

using Sha256Digest = std::array<std::uint8_t, 32>;

class Sha256 final {
public:
    Sha256();

    void update(const void *data, std::size_t size);
    Sha256Digest finish() const;

private:
    void transform(const std::uint8_t block[64]);

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t total_bytes_ = 0;
};

Sha256Digest sha256_bytes(std::string_view bytes);
std::string sha256_hex(const Sha256Digest &digest);
std::string sha256_hex(std::string_view bytes);

bool sha256_file(const std::filesystem::path &path,
                 Sha256Digest *digest,
                 std::uint64_t *byte_count,
                 std::string *error);

bool is_lower_sha256(std::string_view value);

}  // namespace kgv

#endif
