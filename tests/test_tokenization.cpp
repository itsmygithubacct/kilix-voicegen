#include "frontend/tokenization.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kilix_voicegen.h"
#include "runtime/sha256.h"

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

const std::vector<kgv::SegmentDefinition> &segments() {
    static const std::vector<kgv::SegmentDefinition> value = {
        {"K", 1U}, {"AE", 2U}, {"T", 3U},
    };
    return value;
}

const std::vector<std::string> &controls() {
    static const std::vector<std::string> value = {
        "PAD",          "BOS",          "EOS",          "WB",
        "SYL",          "STRESS_0",     "STRESS_1",     "STRESS_2",
        "END_NONE",     "END_COMMA",    "END_COLON",    "END_SEMICOLON",
        "END_PERIOD",   "END_QUESTION", "END_EXCLAMATION",
        "END_PARAGRAPH", "END_CONTINUATION",
    };
    return value;
}

std::string header(std::string_view admission,
                   std::string_view inventory_sha256,
                   std::size_t maximum_input_tokens,
                   std::size_t entry_count = 20U) {
    return "{\"admission\":\"" + std::string(admission) +
           "\",\"dialect\":\"en-AU\",\"entry_count\":" +
           std::to_string(entry_count) +
           ",\"frontend_abi_sha256\":\"" + std::string(64U, 'a') +
           "\",\"maximum_input_tokens\":" +
           std::to_string(maximum_input_tokens) +
           ",\"resource_id\":\"kilix-en-au-token-test-1\"," +
           "\"schema\":\"kilix.voicegen.model-token-inventory/v1\"," +
           "\"segment_inventory_sha256\":\"" +
           std::string(inventory_sha256) + "\"}\n";
}

std::string control_entry(std::size_t token_id, std::string_view name) {
    return "{\"id\":" + std::to_string(token_id) +
           ",\"kind\":\"control\",\"name\":\"" + std::string(name) +
           "\",\"schema\":\"kilix.voicegen.model-token-entry/v1\"," +
           "\"segment_id\":null}\n";
}

std::string segment_entry(std::size_t token_id,
                          std::string_view name,
                          std::size_t segment_id) {
    return "{\"id\":" + std::to_string(token_id) +
           ",\"kind\":\"segment\",\"name\":\"" + std::string(name) +
           "\",\"schema\":\"kilix.voicegen.model-token-entry/v1\"," +
           "\"segment_id\":" + std::to_string(segment_id) + "}\n";
}

std::string fixture(std::string_view inventory_sha256,
                    std::size_t maximum_input_tokens) {
    std::string resource = header("test-fixture", inventory_sha256,
                                  maximum_input_tokens);
    for (std::size_t index = 0U; index < controls().size(); ++index) {
        resource += control_entry(index, controls()[index]);
    }
    resource += segment_entry(17U, "K", 1U);
    resource += segment_entry(18U, "AE", 2U);
    resource += segment_entry(19U, "T", 3U);
    return resource;
}

int load(std::string_view resource,
         kgv::PronunciationAdmission admission,
         std::string_view inventory_sha256,
         kgv::ModelTokenInventory *inventory,
         kgv::ModelTokenFailure *failure) {
    return kgv::load_model_token_inventory(
        resource, kgv::sha256_hex(resource), inventory_sha256, admission,
        segments(), inventory, failure);
}

void expect_load_failure(std::string resource,
                         kgv::PronunciationAdmission admission,
                         std::string_view inventory_sha256,
                         int expected_status,
                         std::string_view expected_code) {
    kgv::ModelTokenInventory inventory;
    kgv::ModelTokenFailure failure;
    const int status = load(resource, admission, inventory_sha256, &inventory,
                            &failure);
    require(status == expected_status, "token inventory returned wrong status");
    require(failure.status == status, "token inventory failure status differs");
    if (failure.code != expected_code) {
        throw std::runtime_error("expected token failure code " +
                                 std::string(expected_code) + ", got " +
                                 failure.code);
    }
    require(inventory.entry_count() == 0U,
            "failed token inventory retained entries");
    require(inventory.resource_id().empty(),
            "failed token inventory retained metadata");
}

kgv::ResolvedTokenWord word(std::size_t start, std::size_t end) {
    kgv::ResolvedTokenWord value;
    value.span = {start, end};
    kgv::PronunciationSyllable syllable;
    syllable.stress = kgv::SyllableStress::primary;
    syllable.segment_ids = {1U, 2U, 3U};
    value.syllables.push_back(std::move(syllable));
    return value;
}

kgv::ResolvedTokenPhrase phrase(std::size_t word_start,
                                std::size_t word_end,
                                kgv::PhraseTerminator terminator,
                                std::size_t byte_start,
                                std::size_t byte_end) {
    return {word_start, word_end, terminator, {byte_start, byte_end}};
}

void require_no_pad(const kgv::ModelTokenResult &result) {
    for (const kgv::ModelTokenChunk &chunk : result.chunks) {
        require(std::find(chunk.ids.begin(), chunk.ids.end(), 0U) ==
                    chunk.ids.end(),
                "PAD was emitted in an unpadded sequence");
    }
}

}  // namespace

int main() {
    try {
        const std::string inventory_sha256 =
            kgv::pronunciation_segment_inventory_sha256(segments());
        require(inventory_sha256.size() == 64U,
                "token test inventory hash was not produced");
        const std::string resource = fixture(inventory_sha256, 32U);

        kgv::ModelTokenInventory inventory;
        kgv::ModelTokenFailure failure;
        require(load(resource, kgv::PronunciationAdmission::test_fixture,
                     inventory_sha256, &inventory, &failure) == KGV_OK,
                "valid token inventory was rejected");
        require(failure.code.empty(), "valid token load retained a failure");
        require(inventory.entry_count() == 20U &&
                    inventory.maximum_input_tokens() == 32U,
                "token inventory dimensions changed");
        require(inventory.resource_sha256() == kgv::sha256_hex(resource) &&
                    inventory.segment_inventory_sha256() == inventory_sha256 &&
                    inventory.frontend_abi_sha256() == std::string(64U, 'a'),
                "token inventory bindings changed");
        require(inventory.control_id("PAD") == 0U &&
                    inventory.control_id("END_PERIOD") == 12U &&
                    inventory.segment_token_id(2U) == 18U &&
                    !inventory.control_id("UNKNOWN") &&
                    !inventory.segment_token_id(99U),
                "token inventory lookup changed");

        {
            std::vector<kgv::ResolvedTokenWord> words = {word(0U, 3U),
                                                        word(4U, 7U)};
            std::vector<kgv::ResolvedTokenPhrase> phrases = {
                phrase(0U, 1U, kgv::PhraseTerminator::comma, 0U, 4U),
                phrase(1U, 2U, kgv::PhraseTerminator::period, 4U, 8U),
            };
            kgv::ModelTokenResult result;
            require(kgv::serialize_model_tokens(inventory, 8U, words, phrases,
                                                &result, &failure) == KGV_OK,
                    "natural token serialization failed");
            require(result.chunks.size() == 1U,
                    "fitting natural phrases were split");
            require(result.chunks[0U].ids ==
                        std::vector<std::uint16_t>({
                            1U, 3U, 4U, 6U, 17U, 18U, 19U, 9U,
                            3U, 4U, 6U, 17U, 18U, 19U, 12U, 2U,
                        }),
                    "canonical token order changed");
            require(!result.chunks[0U].continuation &&
                        result.chunks[0U].source_span.byte_start == 0U &&
                        result.chunks[0U].source_span.byte_end == 8U,
                    "natural chunk metadata changed");
            require_no_pad(result);
            const std::string expected_json =
                "{\"chunks\":[{\"continuation\":false,\"ids\":[1,3,4,6,"
                "17,18,19,9,3,4,6,17,18,19,12,2],\"source_byte_end\":8,"
                "\"source_byte_start\":0}],\"dialect\":\"en-AU\","
                "\"inventory_sha256\":\"" +
                kgv::sha256_hex(resource) +
                "\",\"schema\":\"kilix.voicegen.tokens/v1\"}\n";
            require(kgv::model_token_result_json(result) == expected_json,
                    "model-token JSON changed");
            kgv::ModelTokenResult repeated;
            require(kgv::serialize_model_tokens(inventory, 8U, words, phrases,
                                                &repeated, &failure) == KGV_OK &&
                        kgv::model_token_result_json(repeated) == expected_json,
                    "repeated token serialization was not deterministic");
        }

        {
            kgv::ModelTokenInventory priority_inventory;
            const std::string priority_resource = fixture(inventory_sha256, 23U);
            require(load(priority_resource,
                         kgv::PronunciationAdmission::test_fixture,
                         inventory_sha256, &priority_inventory,
                         &failure) == KGV_OK,
                    "priority inventory failed to load");
            const std::vector<kgv::ResolvedTokenWord> words = {
                word(0U, 3U), word(4U, 7U), word(8U, 11U), word(12U, 15U),
            };
            const std::vector<kgv::ResolvedTokenPhrase> phrases = {
                phrase(0U, 1U, kgv::PhraseTerminator::comma, 0U, 4U),
                phrase(1U, 2U, kgv::PhraseTerminator::paragraph, 4U, 8U),
                phrase(2U, 3U, kgv::PhraseTerminator::comma, 8U, 12U),
                phrase(3U, 4U, kgv::PhraseTerminator::period, 12U, 16U),
            };
            kgv::ModelTokenResult result;
            require(kgv::serialize_model_tokens(priority_inventory, 16U, words,
                                                phrases, &result,
                                                &failure) == KGV_OK,
                    "priority chunking failed");
            require(result.chunks.size() == 2U &&
                        result.chunks[0U].ids[result.chunks[0U].ids.size() - 2U] ==
                            15U &&
                        result.chunks[0U].source_span.byte_end == 8U &&
                        !result.chunks[0U].continuation &&
                        !result.chunks[1U].continuation,
                    "paragraph priority did not beat a later comma");
            require_no_pad(result);
        }

        {
            kgv::ModelTokenInventory forced_inventory;
            const std::string forced_resource = fixture(inventory_sha256, 15U);
            require(load(forced_resource,
                         kgv::PronunciationAdmission::test_fixture,
                         inventory_sha256, &forced_inventory,
                         &failure) == KGV_OK,
                    "forced-split inventory failed to load");
            const std::vector<kgv::ResolvedTokenWord> words = {
                word(0U, 3U), word(4U, 7U), word(8U, 11U),
            };
            const std::vector<kgv::ResolvedTokenPhrase> phrases = {
                phrase(0U, 3U, kgv::PhraseTerminator::period, 0U, 12U),
            };
            kgv::ModelTokenResult result;
            require(kgv::serialize_model_tokens(forced_inventory, 12U, words,
                                                phrases, &result,
                                                &failure) == KGV_OK,
                    "forced word-boundary chunking failed");
            require(result.chunks.size() == 2U &&
                        result.chunks[0U].continuation &&
                        result.chunks[1U].continuation &&
                        result.chunks[0U].ids[result.chunks[0U].ids.size() - 2U] ==
                            16U &&
                        result.chunks[1U].ids[result.chunks[1U].ids.size() - 2U] ==
                            12U,
                    "forced continuation markers or flags changed");
            require_no_pad(result);
        }

        {
            const std::vector<kgv::ResolvedTokenWord> words = {word(0U, 3U),
                                                               word(4U, 7U)};
            const std::vector<kgv::ResolvedTokenPhrase> phrases = {
                phrase(0U, 1U, kgv::PhraseTerminator::continuation, 0U, 4U),
                phrase(1U, 2U, kgv::PhraseTerminator::period, 4U, 8U),
            };
            kgv::ModelTokenResult result;
            require(kgv::serialize_model_tokens(inventory, 8U, words, phrases,
                                                &result, &failure) == KGV_OK &&
                        result.chunks.size() == 2U &&
                        result.chunks[0U].continuation &&
                        result.chunks[1U].continuation &&
                        result.chunks[0U]
                                .ids[result.chunks[0U].ids.size() - 2U] == 16U,
                    "existing continuation boundary was crossed or unmarked");
        }

        {
            kgv::ModelTokenInventory tiny_inventory;
            const std::string tiny_resource = fixture(inventory_sha256, 8U);
            require(load(tiny_resource,
                         kgv::PronunciationAdmission::test_fixture,
                         inventory_sha256, &tiny_inventory,
                         &failure) == KGV_OK,
                    "tiny inventory failed to load");
            kgv::ModelTokenResult result;
            require(kgv::serialize_model_tokens(
                        tiny_inventory, 4U, {word(0U, 3U)},
                        {phrase(0U, 1U, kgv::PhraseTerminator::period, 0U, 4U)},
                        &result, &failure) == KGV_RESOURCE_EXHAUSTED &&
                        failure.code == "MODEL_INPUT_ATOM_TOO_LARGE" &&
                        result.chunks.empty(),
                    "oversized word atom did not fail closed");
        }

        {
            kgv::ResolvedTokenWord two_syllables = word(0U, 3U);
            two_syllables.syllables[0U].segment_ids = {1U};
            kgv::PronunciationSyllable second;
            second.stress = kgv::SyllableStress::none;
            second.segment_ids = {2U, 3U};
            two_syllables.syllables.push_back(second);
            kgv::ModelTokenResult result;
            require(kgv::serialize_model_tokens(
                        inventory, 4U, {two_syllables},
                        {phrase(0U, 1U, kgv::PhraseTerminator::none, 0U, 4U)},
                        &result, &failure) == KGV_OK &&
                        result.chunks[0U].ids ==
                            std::vector<std::uint16_t>({
                                1U, 3U, 4U, 6U, 17U, 4U, 5U, 18U, 19U, 8U,
                                2U,
                            }),
                    "syllable and stress serialization changed");
        }

        {
            kgv::ModelTokenResult empty_result;
            require(kgv::serialize_model_tokens(inventory, 0U, {}, {},
                                                &empty_result,
                                                &failure) == KGV_OK &&
                        empty_result.chunks.empty() &&
                        empty_result.inventory_sha256 ==
                            inventory.resource_sha256(),
                    "empty resolved input was rejected");
        }

        {
            constexpr std::size_t kWordCount = 8192U;
            std::vector<kgv::ResolvedTokenWord> words;
            words.reserve(kWordCount);
            for (std::size_t index = 0U; index < kWordCount; ++index) {
                words.push_back(word(index, index + 1U));
            }
            const std::vector<kgv::ResolvedTokenPhrase> phrases = {
                phrase(0U, kWordCount, kgv::PhraseTerminator::period, 0U,
                       kWordCount),
            };
            kgv::ModelTokenResult result;
            require(kgv::serialize_model_tokens(inventory, kWordCount, words,
                                                phrases, &result,
                                                &failure) == KGV_OK,
                    "large bounded token stream failed");
            std::size_t word_boundaries = 0U;
            for (const kgv::ModelTokenChunk &chunk : result.chunks) {
                require(chunk.ids.size() <= inventory.maximum_input_tokens(),
                        "large-stream chunk exceeded the model budget");
                word_boundaries += static_cast<std::size_t>(
                    std::count(chunk.ids.begin(), chunk.ids.end(), 3U));
            }
            require(word_boundaries == kWordCount,
                    "large token stream dropped or duplicated a word");
        }

        {
            kgv::ResolvedTokenWord invalid = word(0U, 3U);
            invalid.syllables[0U].segment_ids = {99U};
            kgv::ModelTokenResult rejected;
            require(kgv::serialize_model_tokens(
                        inventory, 4U, {invalid},
                        {phrase(0U, 1U, kgv::PhraseTerminator::period, 0U, 4U)},
                        &rejected, &failure) == KGV_ABI_MISMATCH &&
                        failure.code == "MODEL_TOKEN_UNKNOWN_SEGMENT" &&
                        rejected.chunks.empty(),
                    "unknown resolved segment was accepted");
        }

        {
            kgv::ResolvedTokenWord invalid = word(0U, 3U);
            invalid.syllables.push_back(invalid.syllables[0U]);
            kgv::ModelTokenResult rejected;
            require(kgv::serialize_model_tokens(
                        inventory, 4U, {invalid},
                        {phrase(0U, 1U, kgv::PhraseTerminator::period, 0U, 4U)},
                        &rejected, &failure) == KGV_INVALID_ARGUMENT &&
                        failure.code == "MODEL_TOKEN_INVALID_WORD" &&
                        rejected.chunks.empty(),
                    "multiple resolved primary stresses were accepted");
        }

        {
            kgv::ModelTokenResult rejected;
            require(kgv::serialize_model_tokens(
                        inventory, 8U, {word(0U, 3U), word(4U, 7U)},
                        {phrase(0U, 1U, kgv::PhraseTerminator::period, 0U, 4U)},
                        &rejected, &failure) == KGV_INVALID_ARGUMENT &&
                        failure.code == "MODEL_TOKEN_INVALID_PHRASES" &&
                        rejected.inventory_sha256.empty(),
                    "phrase coverage gap was accepted");
        }

        {
            kgv::ModelTokenInventory rejected = inventory;
            kgv::ModelTokenFailure hash_failure;
            const int status = kgv::load_model_token_inventory(
                resource, std::string(64U, '0'), inventory_sha256,
                kgv::PronunciationAdmission::test_fixture, segments(),
                &rejected, &hash_failure);
            require(status == KGV_HASH_MISMATCH &&
                        hash_failure.code == "MODEL_TOKEN_RESOURCE_HASH_MISMATCH" &&
                        rejected.entry_count() == 0U,
                    "token resource hash mismatch did not clear state");
        }
        {
            kgv::ModelTokenInventory rejected;
            kgv::ModelTokenFailure inventory_failure;
            const int status = kgv::load_model_token_inventory(
                resource, kgv::sha256_hex(resource), std::string(64U, 'a'),
                kgv::PronunciationAdmission::test_fixture, segments(),
                &rejected, &inventory_failure);
            require(status == KGV_ABI_MISMATCH &&
                        inventory_failure.code ==
                            "MODEL_TOKEN_SEGMENT_INVENTORY_HASH_MISMATCH",
                    "supplied token inventory mismatch was accepted");
        }
        expect_load_failure(
            fixture(std::string(64U, 'a'), 32U),
            kgv::PronunciationAdmission::test_fixture, inventory_sha256,
            KGV_ABI_MISMATCH, "MODEL_TOKEN_RESOURCE_INVENTORY_MISMATCH");
        expect_load_failure(resource,
                            kgv::PronunciationAdmission::product_admitted,
                            inventory_sha256, KGV_INVALID_MODEL,
                            "MODEL_TOKEN_RESOURCE_ADMISSION");

        std::string duplicate_id = header("test-fixture", inventory_sha256, 32U);
        duplicate_id += control_entry(0U, controls()[0U]);
        duplicate_id += control_entry(0U, controls()[1U]);
        for (std::size_t index = 2U; index < controls().size(); ++index) {
            duplicate_id += control_entry(index, controls()[index]);
        }
        duplicate_id += segment_entry(17U, "K", 1U);
        duplicate_id += segment_entry(18U, "AE", 2U);
        duplicate_id += segment_entry(19U, "T", 3U);
        expect_load_failure(duplicate_id,
                            kgv::PronunciationAdmission::test_fixture,
                            inventory_sha256, KGV_INVALID_MODEL,
                            "MODEL_TOKEN_RESOURCE_ORDER");

        std::string no_final_lf = resource;
        no_final_lf.pop_back();
        expect_load_failure(no_final_lf,
                            kgv::PronunciationAdmission::test_fixture,
                            inventory_sha256, KGV_INVALID_MODEL,
                            "MODEL_TOKEN_RESOURCE_CANONICAL_FORM");

        std::mt19937_64 generator(0x4b47562d544f4b31ULL);
        std::uniform_int_distribution<std::size_t> length_distribution(0U,
                                                                       256U);
        std::uniform_int_distribution<unsigned int> byte_distribution(0U,
                                                                       255U);
        for (std::size_t iteration = 0U; iteration < 2000U; ++iteration) {
            std::string bytes(length_distribution(generator), '\0');
            for (char &byte : bytes) {
                byte = static_cast<char>(byte_distribution(generator));
            }
            kgv::ModelTokenInventory fuzz_inventory;
            kgv::ModelTokenFailure fuzz_failure;
            const int status = load(bytes,
                                    kgv::PronunciationAdmission::test_fixture,
                                    inventory_sha256, &fuzz_inventory,
                                    &fuzz_failure);
            require(status != KGV_OK,
                    "random model-token resource unexpectedly loaded");
            require(fuzz_inventory.entry_count() == 0U,
                    "random failed token resource retained entries");
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
