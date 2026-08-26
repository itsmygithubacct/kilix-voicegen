#ifndef KGV_RUNTIME_MODEL_PACKAGE_H
#define KGV_RUNTIME_MODEL_PACKAGE_H

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "kilix_voicegen.h"

namespace kgv {

struct VerifiedModel final {
    std::filesystem::path directory;
    std::string release_sha256;
    std::string model_id;
    std::string version;
    std::string engine_kind;
    std::array<std::string, 2> voice_ids{};
    std::vector<std::uint16_t> segment_ids;
    std::uint32_t sample_rate = 0U;
    std::string deterministic_test_sha256;
};

int verify_model_package(const std::filesystem::path &directory,
                         std::string_view expected_release_sha256,
                         VerifiedModel *model,
                         std::string *error);

}  // namespace kgv

#endif
