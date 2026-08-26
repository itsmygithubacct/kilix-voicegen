#ifndef KGV_FRONTEND_HETERONYMS_H
#define KGV_FRONTEND_HETERONYMS_H

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "frontend/pipeline.h"
#include "frontend/pronunciation.h"
#include "kilix_voicegen.h"

namespace kgv {

enum class HeteronymCapitalization {
    any,
    lower,
    title,
    upper,
    mixed,
};

enum class HeteronymPosition {
    any,
    clause_start,
    clause_end,
    single_word_clause,
};

enum class HeteronymDecisionKind {
    not_target,
    no_match,
    matched,
    ambiguous,
};

struct HeteronymWordCondition final {
    int offset = 0;
    std::vector<std::string> words;
};

struct HeteronymRule final {
    std::string rule_id;
    std::string target;
    std::string role;
    std::string source;
    HeteronymCapitalization capitalization = HeteronymCapitalization::any;
    HeteronymPosition position = HeteronymPosition::any;
    std::vector<HeteronymWordCondition> conditions;
};

struct HeteronymDecision final {
    HeteronymDecisionKind kind = HeteronymDecisionKind::not_target;
    std::string role;
    std::string rule_id;
    std::size_t match_count = 0U;
};

struct HeteronymResourceFailure final {
    int status = KGV_OK;
    std::string code;
    std::string message;
    std::size_t line = 0U;
};

class HeteronymRules final {
public:
    HeteronymDecision decide(
        const std::vector<LexicalWord> &words,
        std::size_t word_index,
        std::size_t clause_word_start,
        std::size_t clause_word_end) const;
    bool contains_target(std::string_view word) const;
    bool compatible_with(const PronunciationLexicon &lexicon) const;

    const std::string &resource_id() const noexcept;
    const std::string &resource_sha256() const noexcept;
    const std::string &base_lexicon_sha256() const noexcept;
    const std::string &review_record_sha256() const noexcept;
    PronunciationAdmission admission() const noexcept;
    std::size_t rule_count() const noexcept;

private:
    friend int load_heteronym_rules(
        std::string_view,
        std::string_view,
        std::string_view,
        PronunciationAdmission,
        HeteronymRules *,
        HeteronymResourceFailure *);

    std::string resource_id_;
    std::string resource_sha256_;
    std::string base_lexicon_sha256_;
    std::string review_record_sha256_;
    PronunciationAdmission admission_ = PronunciationAdmission::test_fixture;
    std::vector<HeteronymRule> rules_;
    std::map<std::string, std::vector<std::size_t>, std::less<>> index_;
};

int load_heteronym_rules(
    std::string_view jsonl,
    std::string_view expected_resource_sha256,
    std::string_view expected_base_lexicon_sha256,
    PronunciationAdmission required_admission,
    HeteronymRules *rules,
    HeteronymResourceFailure *failure);

const char *heteronym_decision_name(HeteronymDecisionKind value) noexcept;

}  // namespace kgv

#endif
