#ifndef KGV_FRONTEND_FRONTEND_H
#define KGV_FRONTEND_FRONTEND_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "kilix_voicegen.h"

namespace kgv {

struct FrontendScalar final {
    std::uint32_t value = 0U;
    std::size_t byte_start = 0U;
    std::size_t byte_end = 0U;
};

struct FrontendControlSequence final {
    std::size_t byte_start = 0U;
    std::size_t byte_end = 0U;
};

struct FrontendAnalysis final {
    std::size_t scalar_count = 0U;
    std::size_t spoken_scalar_count = 0U;
    std::size_t ignored_control_sequences = 0U;
    std::vector<FrontendScalar> visible_scalars;
    std::vector<FrontendControlSequence> control_sequences;
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

int normalize_frontend_nfc(std::string_view text,
                           const std::vector<FrontendScalar> &input,
                           std::vector<FrontendScalar> *output,
                           FrontendFailure *failure);

bool is_utf8_boundary(std::string_view text, std::size_t offset) noexcept;

}  // namespace kgv

#endif
