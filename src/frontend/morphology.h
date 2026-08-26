#ifndef KGV_FRONTEND_MORPHOLOGY_H
#define KGV_FRONTEND_MORPHOLOGY_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "frontend/pronunciation.h"
#include "kilix_voicegen.h"

namespace kgv {

enum class MorphologyKind {
    plural_or_possessive,
    past,
    progressive,
};

struct MorphologyCandidate final {
    MorphologyKind kind = MorphologyKind::plural_or_possessive;
    std::string stem;
};

struct MorphologyResourceFailure final {
    int status = KGV_OK;
    std::string code;
    std::string message;
};

struct MorphologyApplyFailure final {
    int status = KGV_OK;
    std::string code;
    std::string message;
};

class MorphologyRules final {
public:
    int apply(MorphologyKind kind,
              const std::vector<PronunciationSyllable> &stem,
              std::vector<PronunciationSyllable> *result,
              MorphologyApplyFailure *failure) const;

    const std::string &resource_id() const noexcept;
    const std::string &resource_sha256() const noexcept;
    const std::string &base_lexicon_sha256() const noexcept;
    const std::string &segment_inventory_sha256() const noexcept;
    const std::string &review_record_sha256() const noexcept;
    PronunciationAdmission admission() const noexcept;

private:
    friend int load_morphology_rules(
        std::string_view,
        std::string_view,
        std::string_view,
        std::string_view,
        PronunciationAdmission,
        const std::vector<SegmentDefinition> &,
        MorphologyRules *,
        MorphologyResourceFailure *);

    std::string resource_id_;
    std::string resource_sha256_;
    std::string base_lexicon_sha256_;
    std::string segment_inventory_sha256_;
    std::string review_record_sha256_;
    PronunciationAdmission admission_ = PronunciationAdmission::test_fixture;

    std::vector<std::uint16_t> plural_sibilant_finals_;
    std::vector<std::uint16_t> plural_unvoiced_finals_;
    std::vector<std::uint16_t> plural_sibilant_suffix_;
    std::vector<std::uint16_t> plural_unvoiced_suffix_;
    std::vector<std::uint16_t> plural_voiced_suffix_;
    std::vector<std::uint16_t> past_syllabic_finals_;
    std::vector<std::uint16_t> past_unvoiced_finals_;
    std::vector<std::uint16_t> past_syllabic_suffix_;
    std::vector<std::uint16_t> past_unvoiced_suffix_;
    std::vector<std::uint16_t> past_voiced_suffix_;
    std::vector<std::uint16_t> progressive_suffix_;
    std::vector<std::uint16_t> segment_inventory_ids_;
};

int load_morphology_rules(
    std::string_view json,
    std::string_view expected_resource_sha256,
    std::string_view expected_base_lexicon_sha256,
    std::string_view expected_segment_inventory_sha256,
    PronunciationAdmission required_admission,
    const std::vector<SegmentDefinition> &segments,
    MorphologyRules *rules,
    MorphologyResourceFailure *failure);

std::vector<MorphologyCandidate> morphology_candidates(std::string_view word);
const char *morphology_kind_name(MorphologyKind value) noexcept;

}  // namespace kgv

#endif
