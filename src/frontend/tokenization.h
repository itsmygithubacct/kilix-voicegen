#ifndef KGV_FRONTEND_TOKENIZATION_H
#define KGV_FRONTEND_TOKENIZATION_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "frontend/pipeline.h"
#include "frontend/pronunciation.h"

namespace kgv {

struct ModelTokenFailure final {
    int status = KGV_OK;
    std::string code;
    std::string message;
    std::size_t line = 0U;
};

class ModelTokenInventory final {
public:
    std::optional<std::uint16_t> control_id(std::string_view name) const noexcept;
    std::optional<std::uint16_t> segment_token_id(
        std::uint16_t segment_id) const noexcept;

    const std::string &resource_id() const noexcept;
    const std::string &resource_sha256() const noexcept;
    const std::string &segment_inventory_sha256() const noexcept;
    const std::string &frontend_abi_sha256() const noexcept;
    PronunciationAdmission admission() const noexcept;
    std::size_t entry_count() const noexcept;
    std::size_t maximum_input_tokens() const noexcept;
    std::vector<std::uint16_t> token_ids() const;

private:
    friend int load_model_token_inventory(
        std::string_view,
        std::string_view,
        std::string_view,
        PronunciationAdmission,
        const std::vector<SegmentDefinition> &,
        ModelTokenInventory *,
        ModelTokenFailure *);

    std::string resource_id_;
    std::string resource_sha256_;
    std::string segment_inventory_sha256_;
    std::string frontend_abi_sha256_;
    PronunciationAdmission admission_ = PronunciationAdmission::test_fixture;
    std::size_t maximum_input_tokens_ = 0U;
    std::map<std::string, std::uint16_t, std::less<>> controls_;
    std::map<std::uint16_t, std::uint16_t> segment_tokens_;
};

struct ResolvedTokenWord final {
    SourceSpan span;
    std::vector<PronunciationSyllable> syllables;
};

struct ResolvedTokenPhrase final {
    std::size_t word_start = 0U;
    std::size_t word_end = 0U;
    PhraseTerminator terminator = PhraseTerminator::none;
    SourceSpan span;
};

struct ModelTokenChunk final {
    std::vector<std::uint16_t> ids;
    SourceSpan source_span;
    bool continuation = false;
};

struct ModelTokenResult final {
    std::string inventory_sha256;
    std::vector<ModelTokenChunk> chunks;
};

int load_model_token_inventory(
    std::string_view jsonl,
    std::string_view expected_resource_sha256,
    std::string_view expected_segment_inventory_sha256,
    PronunciationAdmission required_admission,
    const std::vector<SegmentDefinition> &segments,
    ModelTokenInventory *inventory,
    ModelTokenFailure *failure);

int serialize_model_tokens(
    const ModelTokenInventory &inventory,
    std::size_t input_bytes,
    const std::vector<ResolvedTokenWord> &words,
    const std::vector<ResolvedTokenPhrase> &phrases,
    ModelTokenResult *result,
    ModelTokenFailure *failure);

std::string model_token_result_json(const ModelTokenResult &result);

}  // namespace kgv

#endif
