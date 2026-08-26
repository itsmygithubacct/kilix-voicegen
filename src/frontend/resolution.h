#ifndef KGV_FRONTEND_RESOLUTION_H
#define KGV_FRONTEND_RESOLUTION_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "frontend/heteronyms.h"
#include "frontend/lts.h"
#include "frontend/morphology.h"
#include "frontend/overrides.h"
#include "frontend/pipeline.h"
#include "frontend/pronunciation.h"
#include "frontend/tokenization.h"
#include "frontend/weak_forms.h"

namespace kgv {

enum class ResolvedPronunciationSource {
    request_override,
    user_dictionary,
    base_lexicon,
    product_lexicon,
    morphology,
    lts,
    spelling,
};

enum class ResolvedRoleSource {
    default_role,
    explicit_request,
    contextual_rule,
    postlexical_rule,
};

struct ResolvedFrontendWord final {
    std::string normalized;
    std::string source_kind;
    std::string role;
    ResolvedRoleSource role_source = ResolvedRoleSource::default_role;
    std::string context_rule_id;
    bool has_weak_form = false;
    std::string weak_form_rule_id;
    ResolvedPronunciationSource pronunciation_source =
        ResolvedPronunciationSource::base_lexicon;
    bool has_morphology = false;
    MorphologyKind morphology_kind =
        MorphologyKind::plural_or_possessive;
    std::string morphology_stem;
    ResolvedPronunciationSource morphology_stem_source =
        ResolvedPronunciationSource::base_lexicon;
    RequestOverrideKind request_override_kind =
        RequestOverrideKind::replacement_text;
    std::size_t request_override_index = 0U;
    bool has_request_override = false;
    SourceSpan span;
    std::vector<PronunciationSyllable> syllables;
};

struct ResolvedFrontendResources final {
    const PronunciationLexicon *base_lexicon = nullptr;
    const PronunciationLexicon *user_dictionary = nullptr;
    const HeteronymRules *heteronym_rules = nullptr;
    const MorphologyRules *morphology_rules = nullptr;
    const WeakFormRules *weak_form_rules = nullptr;
    const LtsModel *lts = nullptr;
    const ModelTokenInventory *model_tokens = nullptr;
    PronunciationAdmission required_admission =
        PronunciationAdmission::test_fixture;
    std::string expected_frontend_abi_sha256;
};

struct ResolvedFrontendResult final {
    std::string profile;
    std::size_t input_bytes = 0U;
    std::size_t request_override_count = 0U;
    std::string frontend_abi_sha256;
    std::string user_dictionary_sha256;
    std::string heteronym_rules_sha256;
    std::string morphology_rules_sha256;
    std::string weak_form_rules_sha256;
    std::string pronunciation_lexicon_sha256;
    std::string lts_sha256;
    std::vector<ResolvedFrontendWord> words;
    std::vector<ResolvedTokenPhrase> phrases;
    std::vector<FrontendDiagnostic> diagnostics;
    ModelTokenResult model_tokens;
};

struct ResolvedFrontendFailure final {
    int status = KGV_OK;
    std::string code;
    std::string message;
    SourceSpan span;
    std::size_t word_index = 0U;
    bool has_word = false;
    std::size_t override_index = 0U;
    bool has_override = false;
};

int run_resolved_frontend(
    std::string_view text,
    std::uint32_t profile,
    const ResolvedFrontendResources &resources,
    const std::vector<std::string> &word_roles,
    const std::vector<RequestPronunciationOverride> &request_overrides,
    ResolvedFrontendResult *result,
    ResolvedFrontendFailure *failure);

int run_resolved_frontend(
    std::string_view text,
    std::uint32_t profile,
    const ResolvedFrontendResources &resources,
    const std::vector<std::string> &word_roles,
    ResolvedFrontendResult *result,
    ResolvedFrontendFailure *failure);

const char *resolved_pronunciation_source_name(
    ResolvedPronunciationSource value) noexcept;
const char *resolved_role_source_name(ResolvedRoleSource value) noexcept;

}  // namespace kgv

#endif
