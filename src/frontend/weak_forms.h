#ifndef KGV_FRONTEND_WEAK_FORMS_H
#define KGV_FRONTEND_WEAK_FORMS_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "frontend/pronunciation.h"
#include "kilix_voicegen.h"

namespace kgv {

enum class WeakFormCapitalization {
    any,
    lower,
    title,
    upper,
    mixed,
};

enum class WeakFormPosition {
    any,
    phrase_medial,
    phrase_final,
    single_word_phrase,
};

enum class WeakFormNextSegment {
    any,
    vowel,
    non_vowel,
    absent,
};

enum class WeakFormDecisionKind {
    not_target,
    no_match,
    matched,
    ambiguous,
};

struct WeakFormRule final {
    std::string rule_id;
    std::string target;
    std::string role;
    std::string source;
    WeakFormCapitalization capitalization = WeakFormCapitalization::any;
    WeakFormPosition position = WeakFormPosition::any;
    WeakFormNextSegment next_segment = WeakFormNextSegment::any;
};

struct WeakFormDecision final {
    WeakFormDecisionKind kind = WeakFormDecisionKind::not_target;
    std::string role;
    std::string rule_id;
    std::size_t match_count = 0U;
};

struct WeakFormResourceFailure final {
    int status = KGV_OK;
    std::string code;
    std::string message;
    std::size_t line = 0U;
};

class WeakFormRules final {
public:
    WeakFormDecision decide(std::string_view word,
                            std::size_t word_index,
                            std::size_t phrase_word_start,
                            std::size_t phrase_word_end,
                            bool has_next_segment,
                            std::uint16_t next_segment_id) const;
    bool contains_target(std::string_view word) const;
    bool compatible_with(const PronunciationLexicon &lexicon) const;

    const std::string &resource_id() const noexcept;
    const std::string &resource_sha256() const noexcept;
    const std::string &base_lexicon_sha256() const noexcept;
    const std::string &segment_inventory_sha256() const noexcept;
    const std::string &review_record_sha256() const noexcept;
    PronunciationAdmission admission() const noexcept;
    std::size_t rule_count() const noexcept;

private:
    friend int load_weak_form_rules(
        std::string_view,
        std::string_view,
        std::string_view,
        std::string_view,
        PronunciationAdmission,
        const std::vector<SegmentDefinition> &,
        WeakFormRules *,
        WeakFormResourceFailure *);

    std::string resource_id_;
    std::string resource_sha256_;
    std::string base_lexicon_sha256_;
    std::string segment_inventory_sha256_;
    std::string review_record_sha256_;
    PronunciationAdmission admission_ = PronunciationAdmission::test_fixture;
    std::vector<WeakFormRule> rules_;
    std::map<std::string, std::vector<std::size_t>, std::less<>> index_;
    std::vector<std::uint16_t> vowel_segment_ids_;
    std::vector<std::uint16_t> segment_inventory_ids_;
};

int load_weak_form_rules(
    std::string_view jsonl,
    std::string_view expected_resource_sha256,
    std::string_view expected_base_lexicon_sha256,
    std::string_view expected_segment_inventory_sha256,
    PronunciationAdmission required_admission,
    const std::vector<SegmentDefinition> &segments,
    WeakFormRules *rules,
    WeakFormResourceFailure *failure);

const char *weak_form_decision_name(WeakFormDecisionKind value) noexcept;

}  // namespace kgv

#endif
