#include "frontend/resolution.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kilix_voicegen.h"
#include "runtime/sha256.h"

namespace kgv {
namespace {

int reject(ResolvedFrontendResult *result,
           ResolvedFrontendFailure *failure,
           int status,
           std::string code,
           std::string message,
           SourceSpan span = {},
           std::size_t word_index = 0U,
           bool has_word = false,
           std::size_t override_index = 0U,
           bool has_override = false) {
    if (result != nullptr) {
        *result = ResolvedFrontendResult{};
    }
    if (failure != nullptr) {
        failure->status = status;
        failure->code = std::move(code);
        failure->message = std::move(message);
        failure->span = span;
        failure->word_index = word_index;
        failure->has_word = has_word;
        failure->override_index = override_index;
        failure->has_override = has_override;
    }
    return status;
}

bool resource_admission(PronunciationAdmission admission) noexcept {
    return admission == PronunciationAdmission::product_admitted ||
           admission == PronunciationAdmission::test_fixture;
}

bool stable_role(std::string_view role) noexcept {
    if (role.empty() || role.size() > 32U || role.front() < 'a' ||
        role.front() > 'z') {
        return false;
    }
    return std::all_of(role.begin(), role.end(), [](char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '-';
    });
}

bool contains_role(const PronunciationEntry &entry,
                   std::string_view role) noexcept {
    return std::find(entry.roles.begin(), entry.roles.end(), role) !=
           entry.roles.end();
}

bool lts_key(std::string_view grapheme, std::string *key) {
    key->clear();
    if (grapheme.empty() || grapheme.size() > 256U) {
        return false;
    }
    key->reserve(grapheme.size());
    for (std::size_t index = 0U; index < grapheme.size(); ++index) {
        char character = grapheme[index];
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character + ('a' - 'A'));
        }
        const bool letter = character >= 'a' && character <= 'z';
        const bool internal_separator =
            (character == '\'' || character == '-') && index > 0U &&
            index + 1U < grapheme.size();
        if (!letter && !internal_separator) {
            key->clear();
            return false;
        }
        key->push_back(character);
    }
    return true;
}

bool resources_loaded(const ResolvedFrontendResources &resources) noexcept {
    return resources.base_lexicon != nullptr && resources.lts != nullptr &&
           resources.model_tokens != nullptr &&
           !resources.base_lexicon->resource_sha256().empty() &&
           resources.base_lexicon->entry_count() > 0U &&
           !resources.lts->resource_sha256().empty() &&
           resources.lts->node_count() > 0U && resources.lts->root_count() > 0U &&
           !resources.model_tokens->resource_sha256().empty() &&
           resources.model_tokens->entry_count() > 17U;
}

int validate_resources(const ResolvedFrontendResources &resources,
                       ResolvedFrontendResult *result,
                       ResolvedFrontendFailure *failure) {
    if (!resource_admission(resources.required_admission) ||
        !is_lower_sha256(resources.expected_frontend_abi_sha256)) {
        return reject(result, failure, KGV_INVALID_ARGUMENT,
                      "INVALID_RESOLVED_FRONTEND_RESOURCES",
                      "resolved frontend requires a product or test admission and a pinned ABI");
    }
    if (!resources_loaded(resources)) {
        return reject(result, failure, KGV_INVALID_STATE,
                      "RESOLVED_FRONTEND_RESOURCE_NOT_LOADED",
                      "resolved frontend resources are not loaded");
    }
    const PronunciationLexicon &lexicon = *resources.base_lexicon;
    const LtsModel &lts = *resources.lts;
    const ModelTokenInventory &tokens = *resources.model_tokens;
    if (lexicon.admission() != resources.required_admission ||
        lts.admission() != resources.required_admission ||
        tokens.admission() != resources.required_admission) {
        return reject(result, failure, KGV_ABI_MISMATCH,
                      "FRONTEND_RESOURCE_ADMISSION_MISMATCH",
                      "frontend resources do not share the required admission");
    }
    if (lexicon.segment_inventory_sha256().empty() ||
        lexicon.segment_inventory_sha256() !=
            lts.segment_inventory_sha256() ||
        lexicon.segment_inventory_sha256() !=
            tokens.segment_inventory_sha256()) {
        return reject(result, failure, KGV_ABI_MISMATCH,
                      "FRONTEND_SEGMENT_INVENTORY_MISMATCH",
                      "frontend resources do not share one segment inventory");
    }
    if (resources.user_dictionary != nullptr) {
        const PronunciationLexicon &dictionary = *resources.user_dictionary;
        if (dictionary.resource_sha256().empty() ||
            dictionary.entry_count() == 0U) {
            return reject(result, failure, KGV_INVALID_STATE,
                          "FRONTEND_USER_DICTIONARY_NOT_LOADED",
                          "explicit user dictionary is not loaded");
        }
        if (dictionary.admission() !=
            PronunciationAdmission::local_user) {
            return reject(result, failure, KGV_ABI_MISMATCH,
                          "FRONTEND_USER_DICTIONARY_ADMISSION_MISMATCH",
                          "explicit user dictionary is not a local-user resource");
        }
        if (dictionary.segment_inventory_sha256() !=
            lexicon.segment_inventory_sha256()) {
            return reject(result, failure, KGV_ABI_MISMATCH,
                          "FRONTEND_USER_DICTIONARY_INVENTORY_MISMATCH",
                          "user dictionary targets another segment inventory");
        }
        if (!dictionary.review_record_sha256().empty()) {
            return reject(result, failure, KGV_ABI_MISMATCH,
                          "FRONTEND_USER_DICTIONARY_REVIEW_MISMATCH",
                          "local user dictionary must not claim product review");
        }
    }
    if (resources.heteronym_rules != nullptr) {
        const HeteronymRules &rules = *resources.heteronym_rules;
        if (rules.resource_sha256().empty()) {
            return reject(result, failure, KGV_INVALID_STATE,
                          "FRONTEND_HETERONYM_RULES_NOT_LOADED",
                          "contextual heteronym rules are not loaded");
        }
        if (rules.admission() != resources.required_admission) {
            return reject(result, failure, KGV_ABI_MISMATCH,
                          "FRONTEND_HETERONYM_ADMISSION_MISMATCH",
                          "heteronym rules do not share the base admission");
        }
        if (rules.base_lexicon_sha256() != lexicon.resource_sha256()) {
            return reject(result, failure, KGV_ABI_MISMATCH,
                          "FRONTEND_HETERONYM_LEXICON_MISMATCH",
                          "heteronym rules target another base lexicon");
        }
        if (!rules.compatible_with(lexicon)) {
            return reject(result, failure, KGV_ABI_MISMATCH,
                          "FRONTEND_HETERONYM_ROLE_MISMATCH",
                          "heteronym rules lack exact role and default variants in the base lexicon");
        }
        if (resources.required_admission ==
                PronunciationAdmission::product_admitted &&
            rules.review_record_sha256() !=
                lexicon.review_record_sha256()) {
            return reject(result, failure, KGV_ABI_MISMATCH,
                          "FRONTEND_HETERONYM_REVIEW_MISMATCH",
                          "product heteronym rules and lexicon lack one review binding");
        }
    }
    if (lts.source_lexicon_sha256() != lexicon.resource_sha256()) {
        return reject(result, failure, KGV_ABI_MISMATCH,
                      "FRONTEND_LTS_LEXICON_MISMATCH",
                      "LTS model is not bound to the loaded pronunciation lexicon");
    }
    if (tokens.frontend_abi_sha256() !=
        resources.expected_frontend_abi_sha256) {
        return reject(result, failure, KGV_ABI_MISMATCH,
                      "FRONTEND_TOKEN_ABI_MISMATCH",
                      "model-token inventory targets another frontend ABI");
    }
    if (resources.required_admission ==
            PronunciationAdmission::product_admitted &&
        (lexicon.review_record_sha256().empty() ||
         lexicon.review_record_sha256() != lts.review_record_sha256())) {
        return reject(result, failure, KGV_ABI_MISMATCH,
                      "FRONTEND_REVIEW_RECORD_MISMATCH",
                      "product lexicon and LTS model lack one review binding");
    }
    return KGV_OK;
}

ResolvedPronunciationSource lexicon_source(
    PronunciationAdmission admission,
    std::string_view lexical_source_kind,
    bool user_dictionary) noexcept {
    if (user_dictionary) {
        return ResolvedPronunciationSource::user_dictionary;
    }
    if (lexical_source_kind == "SPELLING") {
        return ResolvedPronunciationSource::spelling;
    }
    return admission == PronunciationAdmission::product_admitted
               ? ResolvedPronunciationSource::product_lexicon
               : ResolvedPronunciationSource::base_lexicon;
}

}  // namespace

int run_resolved_frontend(
    std::string_view text,
    std::uint32_t profile,
    const ResolvedFrontendResources &resources,
    const std::vector<std::string> &word_roles,
    const std::vector<RequestPronunciationOverride> &request_overrides,
    ResolvedFrontendResult *result,
    ResolvedFrontendFailure *failure) {
    if (result == nullptr || failure == nullptr) {
        return reject(result, failure, KGV_INVALID_ARGUMENT,
                      "INVALID_RESOLVED_FRONTEND_ARGUMENT",
                      "resolved frontend requires output records");
    }
    *result = ResolvedFrontendResult{};
    *failure = ResolvedFrontendFailure{};
    const int resource_status = validate_resources(resources, result, failure);
    if (resource_status != KGV_OK) {
        return resource_status;
    }

    OverrideLexicalResult overridden_lexical;
    RequestOverrideFailure override_failure;
    const int lexical_status = run_override_lexical_frontend(
        text, profile, request_overrides, *resources.model_tokens,
        &overridden_lexical, &override_failure);
    if (lexical_status != KGV_OK) {
        return reject(result, failure, lexical_status, override_failure.code,
                      override_failure.message, override_failure.span, 0U,
                      false, override_failure.override_index,
                      override_failure.has_override);
    }
    LexicalFrontendResult &lexical = overridden_lexical.lexical;
    if (!word_roles.empty() && word_roles.size() != lexical.words.size()) {
        return reject(result, failure, KGV_INVALID_ARGUMENT,
                      "FRONTEND_ROLE_COUNT_MISMATCH",
                      "word-role count does not match expanded frontend words");
    }

    std::vector<std::pair<std::size_t, std::size_t>> clause_bounds(
        lexical.words.size(), {0U, lexical.words.size()});
    for (const LexicalPhrase &phrase : lexical.phrases) {
        for (std::size_t word_index = phrase.word_start;
             word_index < phrase.word_end &&
             word_index < clause_bounds.size(); ++word_index) {
            clause_bounds[word_index] = {phrase.word_start, phrase.word_end};
        }
    }

    result->profile = lexical.profile;
    result->input_bytes = lexical.input_bytes;
    result->request_override_count = request_overrides.size();
    result->frontend_abi_sha256 = resources.expected_frontend_abi_sha256;
    if (resources.user_dictionary != nullptr) {
        result->user_dictionary_sha256 =
            resources.user_dictionary->resource_sha256();
    }
    if (resources.heteronym_rules != nullptr) {
        result->heteronym_rules_sha256 =
            resources.heteronym_rules->resource_sha256();
    }
    result->pronunciation_lexicon_sha256 =
        resources.base_lexicon->resource_sha256();
    result->lts_sha256 = resources.lts->resource_sha256();
    result->diagnostics = lexical.diagnostics;
    result->words.reserve(lexical.words.size());

    std::vector<ResolvedTokenWord> token_words;
    token_words.reserve(lexical.words.size());
    for (std::size_t index = 0U; index < lexical.words.size(); ++index) {
        const LexicalWord &lexical_word = lexical.words[index];
        const bool explicit_role =
            !word_roles.empty() && !word_roles[index].empty();
        std::string role = explicit_role ? word_roles[index] : "default";
        ResolvedRoleSource role_source =
            explicit_role ? ResolvedRoleSource::explicit_request
                          : ResolvedRoleSource::default_role;
        std::string context_rule_id;
        const bool phone_override =
            overridden_lexical.phone_override_by_word[index].has_value();
        if (!explicit_role && !phone_override &&
            resources.heteronym_rules != nullptr) {
            const HeteronymDecision decision =
                resources.heteronym_rules->decide(
                    lexical.words, index, clause_bounds[index].first,
                    clause_bounds[index].second);
            if (decision.kind == HeteronymDecisionKind::matched) {
                role = decision.role;
                role_source = ResolvedRoleSource::contextual_rule;
                context_rule_id = decision.rule_id;
            } else if (decision.kind ==
                       HeteronymDecisionKind::ambiguous) {
                result->diagnostics.push_back(FrontendDiagnostic{
                    "HETERONYM_RULE_AMBIGUOUS", "WARNING",
                    lexical_word.span,
                });
                result->diagnostics.push_back(FrontendDiagnostic{
                    "HETERONYM_DEFAULTED", "WARNING", lexical_word.span,
                });
            } else if (decision.kind ==
                       HeteronymDecisionKind::no_match) {
                result->diagnostics.push_back(FrontendDiagnostic{
                    "HETERONYM_DEFAULTED", "WARNING", lexical_word.span,
                });
            }
        }
        if (!stable_role(role)) {
            return reject(result, failure, KGV_INVALID_ARGUMENT,
                          "INVALID_FRONTEND_WORD_ROLE",
                          "frontend word role is not canonical",
                          lexical_word.span, index, true);
        }

        ResolvedFrontendWord resolved;
        resolved.normalized = lexical_word.normalized;
        resolved.source_kind = lexical_word.source_kind;
        resolved.role = role;
        resolved.role_source = role_source;
        resolved.context_rule_id = std::move(context_rule_id);
        resolved.span = lexical_word.span;
        if (overridden_lexical.replacement_override_by_word[index]
                .has_value()) {
            resolved.request_override_kind =
                RequestOverrideKind::replacement_text;
            resolved.request_override_index =
                *overridden_lexical.replacement_override_by_word[index];
            resolved.has_request_override = true;
        }
        if (overridden_lexical.phone_override_by_word[index].has_value()) {
            resolved.request_override_kind =
                RequestOverrideKind::phone_syllables;
            resolved.request_override_index =
                *overridden_lexical.phone_override_by_word[index];
            resolved.has_request_override = true;
            resolved.syllables = request_overrides[
                resolved.request_override_index].syllables;
            resolved.pronunciation_source =
                ResolvedPronunciationSource::request_override;
        }
        const PronunciationLexicon *selected_lexicon = nullptr;
        const PronunciationEntry *entry = nullptr;
        if (resolved.pronunciation_source !=
                ResolvedPronunciationSource::request_override &&
            resources.user_dictionary != nullptr) {
            entry = resources.user_dictionary->find(lexical_word.normalized,
                                                     role);
            if (entry != nullptr) {
                selected_lexicon = resources.user_dictionary;
            }
        }
        if (resolved.pronunciation_source !=
                ResolvedPronunciationSource::request_override &&
            entry == nullptr) {
            entry = resources.base_lexicon->find(lexical_word.normalized, role);
            if (entry != nullptr) {
                selected_lexicon = resources.base_lexicon;
            }
        }
        if (resolved.pronunciation_source ==
            ResolvedPronunciationSource::request_override) {
            // The typed request pronunciation is already validated against
            // the exact model inventory and wins over every persistent layer.
        } else if (entry != nullptr) {
            resolved.syllables = entry->syllables;
            resolved.pronunciation_source = lexicon_source(
                resources.required_admission, lexical_word.source_kind,
                selected_lexicon == resources.user_dictionary);
            if (role != "default" && !contains_role(*entry, role)) {
                result->diagnostics.push_back(FrontendDiagnostic{
                    "HETERONYM_DEFAULTED", "WARNING", lexical_word.span,
                });
            }
        } else {
            const bool user_knows_grapheme =
                resources.user_dictionary != nullptr &&
                resources.user_dictionary->contains_grapheme(
                    lexical_word.normalized);
            if (user_knows_grapheme ||
                resources.base_lexicon->contains_grapheme(
                    lexical_word.normalized)) {
                return reject(result, failure, KGV_INVALID_TEXT,
                              "AMBIGUOUS_PRONUNCIATION_ROLE",
                              "known word lacks a reviewed pronunciation for its role",
                              lexical_word.span, index, true,
                              resolved.request_override_index,
                              resolved.has_request_override);
            }
            if (lexical_word.source_kind == "SPELLING") {
                return reject(result, failure, KGV_INVALID_TEXT,
                              "UNKNOWN_PRONUNCIATION",
                              "spelled word lacks an admitted letter-name pronunciation",
                              lexical_word.span, index, true,
                              resolved.request_override_index,
                              resolved.has_request_override);
            }
            std::string key;
            if (!lts_key(lexical_word.normalized, &key)) {
                return reject(result, failure, KGV_INVALID_TEXT,
                              "UNKNOWN_PRONUNCIATION",
                              "word has no admitted lexicon or LTS pronunciation",
                              lexical_word.span, index, true,
                              resolved.request_override_index,
                              resolved.has_request_override);
            }
            LtsFailure lts_failure;
            const int lts_status =
                resources.lts->pronounce(key, &resolved.syllables, &lts_failure);
            if (lts_status != KGV_OK) {
                if (lts_status == KGV_INVALID_TEXT) {
                    return reject(result, failure, KGV_INVALID_TEXT,
                                  "UNKNOWN_PRONUNCIATION",
                                  "word has no admitted lexicon or LTS pronunciation",
                                  lexical_word.span, index, true,
                                  resolved.request_override_index,
                                  resolved.has_request_override);
                }
                return reject(result, failure, lts_status, lts_failure.code,
                              lts_failure.message, lexical_word.span, index,
                              true, resolved.request_override_index,
                              resolved.has_request_override);
            }
            resolved.pronunciation_source = ResolvedPronunciationSource::lts;
        }

        ResolvedTokenWord token_word;
        token_word.span = resolved.span;
        token_word.syllables = resolved.syllables;
        token_words.push_back(std::move(token_word));
        result->words.push_back(std::move(resolved));
    }

    result->phrases.reserve(lexical.phrases.size());
    for (const LexicalPhrase &phrase : lexical.phrases) {
        if (phrase.word_start == phrase.word_end) {
            continue;
        }
        result->phrases.push_back(ResolvedTokenPhrase{
            phrase.word_start,
            phrase.word_end,
            phrase.terminator,
            phrase.span,
        });
    }

    ModelTokenFailure token_failure;
    const int token_status = serialize_model_tokens(
        *resources.model_tokens, lexical.input_bytes, token_words,
        result->phrases, &result->model_tokens, &token_failure);
    if (token_status != KGV_OK) {
        return reject(result, failure, token_status, token_failure.code,
                      token_failure.message);
    }
    return KGV_OK;
}

const char *resolved_pronunciation_source_name(
    ResolvedPronunciationSource value) noexcept {
    switch (value) {
        case ResolvedPronunciationSource::request_override:
            return "REQUEST_OVERRIDE";
        case ResolvedPronunciationSource::user_dictionary:
            return "USER_DICTIONARY";
        case ResolvedPronunciationSource::base_lexicon:
            return "BASE_LEXICON";
        case ResolvedPronunciationSource::product_lexicon:
            return "PRODUCT_LEXICON";
        case ResolvedPronunciationSource::lts: return "LTS";
        case ResolvedPronunciationSource::spelling: return "SPELLING";
    }
    return "UNKNOWN";
}

const char *resolved_role_source_name(ResolvedRoleSource value) noexcept {
    switch (value) {
        case ResolvedRoleSource::default_role: return "DEFAULT";
        case ResolvedRoleSource::explicit_request: return "EXPLICIT_REQUEST";
        case ResolvedRoleSource::contextual_rule: return "CONTEXTUAL_RULE";
    }
    return "UNKNOWN";
}

int run_resolved_frontend(
    std::string_view text,
    std::uint32_t profile,
    const ResolvedFrontendResources &resources,
    const std::vector<std::string> &word_roles,
    ResolvedFrontendResult *result,
    ResolvedFrontendFailure *failure) {
    static const std::vector<RequestPronunciationOverride> no_overrides;
    return run_resolved_frontend(text, profile, resources, word_roles,
                                 no_overrides, result, failure);
}

}  // namespace kgv
