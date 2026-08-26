#ifndef KGV_FRONTEND_LTS_H
#define KGV_FRONTEND_LTS_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "frontend/pronunciation.h"

namespace kgv {

struct LtsFailure final {
    int status = KGV_OK;
    std::string code;
    std::string message;
    std::size_t line = 0U;
};

class LtsModel final {
public:
    int pronounce(std::string_view grapheme,
                  std::vector<PronunciationSyllable> *syllables,
                  LtsFailure *failure) const;

    const std::string &resource_id() const noexcept;
    const std::string &resource_sha256() const noexcept;
    const std::string &segment_inventory_sha256() const noexcept;
    const std::string &source_lexicon_sha256() const noexcept;
    const std::string &training_record_sha256() const noexcept;
    const std::string &review_record_sha256() const noexcept;
    PronunciationAdmission admission() const noexcept;
    std::size_t node_count() const noexcept;
    std::size_t root_count() const noexcept;

private:
    struct Emission final {
        std::vector<std::uint16_t> segment_ids;
        bool ends_syllable = false;
        SyllableStress stress = SyllableStress::none;
    };

    struct Node final {
        bool leaf = false;
        std::int8_t feature_offset = 0;
        char feature_value = '\0';
        std::uint32_t yes = 0U;
        std::uint32_t no = 0U;
        std::vector<Emission> emissions;
    };

    friend int load_lts_model(
        std::string_view,
        std::string_view,
        std::string_view,
        PronunciationAdmission,
        const std::vector<SegmentDefinition> &,
        LtsModel *,
        LtsFailure *);

    std::string resource_id_;
    std::string resource_sha256_;
    std::string segment_inventory_sha256_;
    std::string source_lexicon_sha256_;
    std::string training_record_sha256_;
    std::string review_record_sha256_;
    PronunciationAdmission admission_ = PronunciationAdmission::test_fixture;
    std::uint32_t maximum_steps_ = 0U;
    std::map<char, std::uint32_t> roots_;
    std::vector<Node> nodes_;
};

int load_lts_model(
    std::string_view jsonl,
    std::string_view expected_resource_sha256,
    std::string_view expected_segment_inventory_sha256,
    PronunciationAdmission required_admission,
    const std::vector<SegmentDefinition> &segments,
    LtsModel *model,
    LtsFailure *failure);

}  // namespace kgv

#endif
