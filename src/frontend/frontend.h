#ifndef KGV_FRONTEND_FRONTEND_H
#define KGV_FRONTEND_FRONTEND_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "kilix_voicegen.h"

namespace kgv {

struct FrontendAnalysis final {
    std::size_t scalar_count = 0U;
    std::size_t spoken_scalar_count = 0U;
    std::size_t ignored_control_sequences = 0U;
};

struct FrontendFailure final {
    int status = KGV_OK;
    std::string code;
    std::string message;
    std::size_t byte_offset = 0U;
};

int analyze_frontend(std::string_view text,
                     std::uint32_t profile,
                     FrontendAnalysis *analysis,
                     FrontendFailure *failure);

bool is_utf8_boundary(std::string_view text, std::size_t offset) noexcept;

}  // namespace kgv

#endif
