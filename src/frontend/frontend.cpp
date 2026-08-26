#include "frontend/frontend.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <sstream>

#include <utf8proc.h>

namespace kgv {
namespace {

struct Decoded final {
    std::uint32_t scalar = 0U;
    std::size_t size = 0U;
};

bool continuation(unsigned char byte) noexcept {
    return (byte & 0xc0U) == 0x80U;
}

bool decode_one(std::string_view text, std::size_t offset, Decoded *decoded) noexcept {
    if (decoded == nullptr || offset >= text.size()) {
        return false;
    }
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first <= 0x7fU) {
        decoded->scalar = first;
        decoded->size = 1U;
        return true;
    }
    if (first >= 0xc2U && first <= 0xdfU) {
        if (offset + 1U >= text.size()) {
            return false;
        }
        const auto second = static_cast<unsigned char>(text[offset + 1U]);
        if (!continuation(second)) {
            return false;
        }
        decoded->scalar = ((static_cast<std::uint32_t>(first) & 0x1fU) << 6U) |
                          (static_cast<std::uint32_t>(second) & 0x3fU);
        decoded->size = 2U;
        return true;
    }
    if (first >= 0xe0U && first <= 0xefU) {
        if (offset + 2U >= text.size()) {
            return false;
        }
        const auto second = static_cast<unsigned char>(text[offset + 1U]);
        const auto third = static_cast<unsigned char>(text[offset + 2U]);
        if (!continuation(second) || !continuation(third) ||
            (first == 0xe0U && second < 0xa0U) ||
            (first == 0xedU && second >= 0xa0U)) {
            return false;
        }
        decoded->scalar = ((static_cast<std::uint32_t>(first) & 0x0fU) << 12U) |
                          ((static_cast<std::uint32_t>(second) & 0x3fU) << 6U) |
                          (static_cast<std::uint32_t>(third) & 0x3fU);
        decoded->size = 3U;
        return true;
    }
    if (first >= 0xf0U && first <= 0xf4U) {
        if (offset + 3U >= text.size()) {
            return false;
        }
        const auto second = static_cast<unsigned char>(text[offset + 1U]);
        const auto third = static_cast<unsigned char>(text[offset + 2U]);
        const auto fourth = static_cast<unsigned char>(text[offset + 3U]);
        if (!continuation(second) || !continuation(third) || !continuation(fourth) ||
            (first == 0xf0U && second < 0x90U) ||
            (first == 0xf4U && second >= 0x90U)) {
            return false;
        }
        decoded->scalar = ((static_cast<std::uint32_t>(first) & 0x07U) << 18U) |
                          ((static_cast<std::uint32_t>(second) & 0x3fU) << 12U) |
                          ((static_cast<std::uint32_t>(third) & 0x3fU) << 6U) |
                          (static_cast<std::uint32_t>(fourth) & 0x3fU);
        decoded->size = 4U;
        return true;
    }
    return false;
}

bool bidi_control(std::uint32_t scalar) noexcept {
    return scalar == 0x061cU || scalar == 0x200eU || scalar == 0x200fU ||
           (scalar >= 0x202aU && scalar <= 0x202eU) ||
           (scalar >= 0x2066U && scalar <= 0x2069U);
}

bool permitted_whitespace(std::uint32_t scalar) noexcept {
    return scalar == 0x0009U || scalar == 0x000aU || scalar == 0x000dU ||
           scalar == 0x0020U || scalar == 0x00a0U ||
           scalar == 0x1680U || (scalar >= 0x2000U && scalar <= 0x200aU) ||
           scalar == 0x2028U || scalar == 0x2029U || scalar == 0x202fU ||
           scalar == 0x205fU || scalar == 0x3000U;
}

bool unsupported_control(std::uint32_t scalar) noexcept {
    return (scalar <= 0x001fU) || (scalar >= 0x007fU && scalar <= 0x009fU);
}

std::string offset_message(std::string_view description, std::size_t offset) {
    std::ostringstream message;
    message << description << " at UTF-8 byte offset " << offset;
    return message.str();
}

int reject(FrontendFailure *failure, int status, std::string code,
           std::string message, std::size_t offset) {
    if (failure != nullptr) {
        failure->status = status;
        failure->code = std::move(code);
        failure->message = std::move(message);
        failure->byte_offset = offset;
    }
    return status;
}

bool scan_csi(std::string_view text, std::size_t content_start,
              std::size_t *next) noexcept {
    constexpr std::size_t kMaximumSequenceBytes = 4096U;
    if (content_start > text.size()) {
        return false;
    }
    const std::size_t limit =
        content_start + std::min(kMaximumSequenceBytes, text.size() - content_start);
    std::size_t offset = content_start;
    bool saw_intermediate = false;
    while (offset < text.size() && offset < limit) {
        const auto byte = static_cast<unsigned char>(text[offset]);
        if (byte >= 0x40U && byte <= 0x7eU) {
            *next = offset + 1U;
            return true;
        }
        if (byte >= 0x20U && byte <= 0x2fU) {
            saw_intermediate = true;
        } else if (byte < 0x30U || byte > 0x3fU || saw_intermediate) {
            return false;
        }
        ++offset;
    }
    return false;
}

bool scan_osc(std::string_view text, std::size_t content_start,
              std::size_t *next) noexcept {
    constexpr std::size_t kMaximumSequenceBytes = 4096U;
    std::size_t offset = content_start;
    std::size_t examined = 0U;
    while (offset < text.size() && examined < kMaximumSequenceBytes) {
        const auto byte = static_cast<unsigned char>(text[offset]);
        if (byte == 0x07U) {
            *next = offset + 1U;
            return true;
        }
        if (byte == 0x1bU && offset + 1U < text.size() && text[offset + 1U] == '\\') {
            *next = offset + 2U;
            return true;
        }
        ++offset;
        ++examined;
    }
    return false;
}

}  // namespace

int analyze_frontend(std::string_view text,
                     std::uint32_t profile,
                     FrontendAnalysis *analysis,
                     FrontendFailure *failure) {
    if (analysis == nullptr || failure == nullptr) {
        return KGV_INVALID_ARGUMENT;
    }
    *analysis = FrontendAnalysis{};
    *failure = FrontendFailure{};

    if (profile != KGV_PROFILE_PROSE && profile != KGV_PROFILE_TERMINAL) {
        return reject(failure, KGV_INVALID_ARGUMENT, "INVALID_PROFILE",
                      "text profile must be prose or terminal", 0U);
    }
    if (text.size() > KGV_MAX_INPUT_BYTES) {
        return reject(failure, KGV_INPUT_TOO_LARGE, "INPUT_TOO_LARGE",
                      "UTF-8 input exceeds the 65536-byte runtime limit", 0U);
    }

    std::size_t offset = 0U;
    while (offset < text.size()) {
        Decoded decoded{};
        if (!decode_one(text, offset, &decoded)) {
            return reject(failure, KGV_INVALID_TEXT, "INVALID_UTF8",
                          offset_message("malformed UTF-8", offset), offset);
        }
        if (decoded.scalar == 0U) {
            return reject(failure, KGV_INVALID_TEXT, "NUL_IN_INPUT",
                          offset_message("U+0000 is not accepted", offset), offset);
        }
        if (bidi_control(decoded.scalar)) {
            return reject(failure, KGV_INVALID_TEXT, "BIDI_CONTROL",
                          offset_message("bidirectional format control is not accepted", offset),
                          offset);
        }
        ++analysis->scalar_count;
        offset += decoded.size;
    }

    offset = 0U;
    while (offset < text.size()) {
        Decoded decoded{};
        if (!decode_one(text, offset, &decoded)) {
            return reject(failure, KGV_INTERNAL_ERROR, "VALIDATION_DRIFT",
                          "validated UTF-8 could not be decoded", offset);
        }

        if (profile == KGV_PROFILE_TERMINAL && decoded.scalar == 0x001bU) {
            if (offset + 1U >= text.size()) {
                return reject(failure, KGV_INVALID_TEXT, "UNSUPPORTED_CONTROL",
                              offset_message("incomplete terminal escape sequence", offset),
                              offset);
            }
            std::size_t next = 0U;
            if (text[offset + 1U] == '[' && scan_csi(text, offset + 2U, &next)) {
                ++analysis->ignored_control_sequences;
                analysis->control_sequences.push_back(
                    FrontendControlSequence{offset, next});
                offset = next;
                continue;
            }
            if (text[offset + 1U] == ']' && scan_osc(text, offset + 2U, &next)) {
                ++analysis->ignored_control_sequences;
                analysis->control_sequences.push_back(
                    FrontendControlSequence{offset, next});
                offset = next;
                continue;
            }
            return reject(failure, KGV_INVALID_TEXT, "UNSUPPORTED_CONTROL",
                          offset_message("unsupported terminal escape sequence", offset), offset);
        }
        if (profile == KGV_PROFILE_TERMINAL && decoded.scalar == 0x009bU) {
            std::size_t next = 0U;
            if (scan_csi(text, offset + decoded.size, &next)) {
                ++analysis->ignored_control_sequences;
                analysis->control_sequences.push_back(
                    FrontendControlSequence{offset, next});
                offset = next;
                continue;
            }
            return reject(failure, KGV_INVALID_TEXT, "UNSUPPORTED_CONTROL",
                          offset_message("incomplete terminal CSI sequence", offset), offset);
        }
        if (profile == KGV_PROFILE_TERMINAL && decoded.scalar == 0x009dU) {
            std::size_t next = 0U;
            if (scan_osc(text, offset + decoded.size, &next)) {
                ++analysis->ignored_control_sequences;
                analysis->control_sequences.push_back(
                    FrontendControlSequence{offset, next});
                offset = next;
                continue;
            }
            return reject(failure, KGV_INVALID_TEXT, "UNSUPPORTED_CONTROL",
                          offset_message("incomplete terminal OSC sequence", offset), offset);
        }
        if (unsupported_control(decoded.scalar) && !permitted_whitespace(decoded.scalar)) {
            return reject(failure, KGV_INVALID_TEXT, "UNSUPPORTED_CONTROL",
                          offset_message("unsupported control character", offset), offset);
        }
        if (!permitted_whitespace(decoded.scalar)) {
            ++analysis->spoken_scalar_count;
        }
        analysis->visible_scalars.push_back(
            FrontendScalar{decoded.scalar, offset, offset + decoded.size});
        offset += decoded.size;
    }
    return KGV_OK;
}

bool is_utf8_boundary(std::string_view text, std::size_t offset) noexcept {
    if (offset == 0U || offset == text.size()) {
        return true;
    }
    if (offset > text.size()) {
        return false;
    }
    return !continuation(static_cast<unsigned char>(text[offset]));
}

int normalize_frontend_nfc(std::string_view text,
                           const std::vector<FrontendScalar> &input,
                           std::vector<FrontendScalar> *output,
                           FrontendFailure *failure) {
    if (output == nullptr || failure == nullptr) {
        return KGV_INVALID_ARGUMENT;
    }
    output->clear();
    if (input.empty()) {
        return KGV_OK;
    }

    auto normalize_cluster = [&](std::size_t begin, std::size_t end) -> int {
        std::string cluster;
        for (std::size_t index = begin; index < end; ++index) {
            const FrontendScalar &scalar = input[index];
            cluster.append(text.substr(scalar.byte_start, scalar.byte_end - scalar.byte_start));
        }
        if (cluster.size() > static_cast<std::size_t>(
                                 std::numeric_limits<utf8proc_ssize_t>::max())) {
            return reject(failure, KGV_INPUT_TOO_LARGE, "NORMALIZATION_INPUT_TOO_LARGE",
                          "normalization cluster exceeds the supported size", input[begin].byte_start);
        }
        utf8proc_uint8_t *mapped = nullptr;
        const utf8proc_ssize_t mapped_size = utf8proc_map(
            reinterpret_cast<const utf8proc_uint8_t *>(cluster.data()),
            static_cast<utf8proc_ssize_t>(cluster.size()), &mapped,
            static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE));
        if (mapped_size < 0 || mapped == nullptr) {
            std::free(mapped);
            return reject(failure, KGV_INTERNAL_ERROR, "NFC_NORMALIZATION_FAILED",
                          "utf8proc could not normalize validated UTF-8", input[begin].byte_start);
        }
        const std::size_t cluster_byte_start = input[begin].byte_start;
        const std::size_t cluster_byte_end = input[end - 1U].byte_end;
        utf8proc_ssize_t offset = 0;
        while (offset < mapped_size) {
            utf8proc_int32_t codepoint = 0;
            const utf8proc_ssize_t consumed = utf8proc_iterate(
                mapped + offset, mapped_size - offset, &codepoint);
            if (consumed <= 0) {
                std::free(mapped);
                return reject(failure, KGV_INTERNAL_ERROR, "NFC_NORMALIZATION_DRIFT",
                              "utf8proc emitted invalid normalized UTF-8", input[begin].byte_start);
            }
            output->push_back(FrontendScalar{
                static_cast<std::uint32_t>(codepoint),
                cluster_byte_start,
                cluster_byte_end,
            });
            offset += consumed;
        }
        std::free(mapped);
        return KGV_OK;
    };

    std::size_t cluster_begin = 0U;
    utf8proc_int32_t grapheme_state = 0;
    for (std::size_t index = 1U; index < input.size(); ++index) {
        const bool discontinuity = input[index - 1U].byte_end != input[index].byte_start;
        bool boundary = discontinuity;
        if (!discontinuity) {
            boundary = utf8proc_grapheme_break_stateful(
                           static_cast<utf8proc_int32_t>(input[index - 1U].value),
                           static_cast<utf8proc_int32_t>(input[index].value),
                           &grapheme_state) != 0;
        }
        if (boundary) {
            const int status = normalize_cluster(cluster_begin, index);
            if (status != KGV_OK) {
                return status;
            }
            cluster_begin = index;
            grapheme_state = 0;
        }
    }
    return normalize_cluster(cluster_begin, input.size());
}

}  // namespace kgv
