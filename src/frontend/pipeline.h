#ifndef KGV_FRONTEND_PIPELINE_H
#define KGV_FRONTEND_PIPELINE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "frontend/frontend.h"

namespace kgv {

struct SourceSpan final {
    std::size_t byte_start = 0U;
    std::size_t byte_end = 0U;
};

struct FrontendDiagnostic final {
    std::string code;
    std::string severity;
    SourceSpan span;
};

struct LexicalWord final {
    std::string normalized;
    std::string source_kind;
    SourceSpan span;
};

enum class PhraseTerminator {
    none,
    comma,
    colon,
    semicolon,
    period,
    question,
    exclamation,
    paragraph,
    continuation,
};

struct LexicalPhrase final {
    std::size_t word_start = 0U;
    std::size_t word_end = 0U;
    PhraseTerminator terminator = PhraseTerminator::none;
    SourceSpan span;
};

struct LexicalFrontendResult final {
    std::string profile;
    std::size_t input_bytes = 0U;
    std::size_t ignored_control_sequences = 0U;
    std::vector<LexicalWord> words;
    std::vector<LexicalPhrase> phrases;
    std::vector<FrontendDiagnostic> diagnostics;
};

int run_lexical_frontend(std::string_view text,
                         std::uint32_t profile,
                         LexicalFrontendResult *result,
                         FrontendFailure *failure);

std::string lexical_frontend_json(const LexicalFrontendResult &result,
                                  int status,
                                  const FrontendFailure &failure);

const char *phrase_terminator_name(PhraseTerminator value) noexcept;

}  // namespace kgv

#endif
