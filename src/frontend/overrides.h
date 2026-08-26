#ifndef KGV_FRONTEND_OVERRIDES_H
#define KGV_FRONTEND_OVERRIDES_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "frontend/pipeline.h"
#include "frontend/pronunciation.h"
#include "frontend/tokenization.h"

namespace kgv {

enum class RequestOverrideKind {
    replacement_text,
    phone_syllables,
};

struct RequestPronunciationOverride final {
    SourceSpan span;
    RequestOverrideKind kind = RequestOverrideKind::replacement_text;
    std::string replacement_text;
    std::vector<PronunciationSyllable> syllables;
};

struct OverrideLexicalResult final {
    LexicalFrontendResult lexical;
    std::vector<std::optional<std::size_t>> phone_override_by_word;
    std::vector<std::optional<std::size_t>> replacement_override_by_word;
};

struct RequestOverrideFailure final {
    int status = KGV_OK;
    std::string code;
    std::string message;
    SourceSpan span;
    std::size_t override_index = 0U;
    bool has_override = false;
};

int run_override_lexical_frontend(
    std::string_view text,
    std::uint32_t profile,
    const std::vector<RequestPronunciationOverride> &overrides,
    const ModelTokenInventory &model_tokens,
    OverrideLexicalResult *result,
    RequestOverrideFailure *failure);

}  // namespace kgv

#endif
