#ifndef KGV_FRONTEND_PRONUNCIATION_H
#define KGV_FRONTEND_PRONUNCIATION_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "kilix_voicegen.h"

namespace kgv {

enum class PronunciationAdmission {
    product_admitted,
    local_user,
    test_fixture,
};

enum class SyllableStress {
    none,
    primary,
    secondary,
};

struct SegmentDefinition final {
    std::string name;
    std::uint16_t id = 0U;
};

struct PronunciationSyllable final {
    SyllableStress stress = SyllableStress::none;
    std::vector<std::uint16_t> segment_ids;
};

struct PronunciationEntry final {
    std::string grapheme;
    std::string case_mode;
    std::vector<std::string> roles;
    std::vector<PronunciationSyllable> syllables;
    std::string source;
};

struct PronunciationResourceFailure final {
    int status = KGV_OK;
    std::string code;
    std::string message;
    std::size_t line = 0U;
};

class PronunciationLexicon final {
public:
    const PronunciationEntry *find(std::string_view grapheme,
                                   std::string_view role) const;
    bool contains_grapheme(std::string_view grapheme) const;
    bool contains_role(std::string_view grapheme,
                       std::string_view role) const;

    const std::string &resource_id() const noexcept;
    const std::string &resource_sha256() const noexcept;
    const std::string &segment_inventory_sha256() const noexcept;
    const std::string &review_record_sha256() const noexcept;
    PronunciationAdmission admission() const noexcept;
    std::size_t entry_count() const noexcept;

private:
    friend int load_pronunciation_lexicon(
        std::string_view,
        std::string_view,
        std::string_view,
        PronunciationAdmission,
        const std::vector<SegmentDefinition> &,
        PronunciationLexicon *,
        PronunciationResourceFailure *);

    std::string resource_id_;
    std::string resource_sha256_;
    std::string segment_inventory_sha256_;
    std::string review_record_sha256_;
    PronunciationAdmission admission_ = PronunciationAdmission::test_fixture;
    std::vector<PronunciationEntry> entries_;
    std::map<std::string, std::size_t, std::less<>> index_;
};

int load_pronunciation_lexicon(
    std::string_view jsonl,
    std::string_view expected_resource_sha256,
    std::string_view expected_segment_inventory_sha256,
    PronunciationAdmission required_admission,
    const std::vector<SegmentDefinition> &segments,
    PronunciationLexicon *lexicon,
    PronunciationResourceFailure *failure);

std::string pronunciation_segment_inventory_sha256(
    const std::vector<SegmentDefinition> &segments);

const char *pronunciation_admission_name(PronunciationAdmission value) noexcept;
const char *syllable_stress_name(SyllableStress value) noexcept;

}  // namespace kgv

#endif
