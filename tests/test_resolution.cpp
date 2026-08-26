#include "frontend/resolution.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "kilix_voicegen.h"
#include "runtime/sha256.h"

namespace {

constexpr std::string_view kFrontendAbi =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

const std::vector<kgv::SegmentDefinition> &segments() {
    static const std::vector<kgv::SegmentDefinition> value = {
        {"K", 1U}, {"AE", 2U}, {"T", 3U}, {"B", 4U},
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

std::string admission_name(kgv::PronunciationAdmission admission) {
    return kgv::pronunciation_admission_name(admission);
}

std::string review_json(std::string_view review) {
    return review.empty() ? "null" : "\"" + std::string(review) + "\"";
}

std::string pronunciation_entry(std::string_view grapheme,
                                std::string_view roles,
                                std::string_view syllables) {
    return "{\"case\":\"ascii-fold\",\"grapheme\":\"" +
           std::string(grapheme) + "\",\"roles\":" + std::string(roles) +
           ",\"schema\":\"kilix.voicegen.pronunciation-entry/v1\"," +
           "\"source\":\"project-test-fixture\",\"syllables\":" +
           std::string(syllables) + "}\n";
}

std::string pronunciation_fixture(
    kgv::PronunciationAdmission admission,
    std::string_view inventory_sha256,
    std::string_view review) {
    std::string resource =
        "{\"admission\":\"" + admission_name(admission) +
        "\",\"dialect\":\"en-AU\",\"entry_count\":3," +
        "\"resource_id\":\"kilix-en-au-resolution-lexicon-test-1\"," +
        "\"review_record_sha256\":" + review_json(review) +
        ",\"schema\":\"kilix.voicegen.pronunciation-lexicon/v1\"," +
        "\"segment_inventory_sha256\":\"" +
        std::string(inventory_sha256) + "\"}\n";
    resource += pronunciation_entry(
        "record", "[\"noun\"]",
        "[{\"segments\":[\"K\"],\"stress\":\"primary\"}]");
    resource += pronunciation_entry(
        "record", "[\"verb\"]",
        "[{\"segments\":[\"B\"],\"stress\":\"primary\"}]");
    resource += pronunciation_entry(
        "cat", "[\"default\"]",
        "[{\"segments\":[\"K\",\"AE\",\"T\"],"
        "\"stress\":\"primary\"}]");
    return resource;
}

std::string lts_roots(bool product) {
    if (!product) {
        return "{\"a\":0,\"b\":1,\"c\":2,\"t\":3}";
    }
    std::string roots = "{";
    for (char symbol = 'a'; symbol <= 'z'; ++symbol) {
        if (symbol != 'a') roots.push_back(',');
        std::size_t node = 0U;
        if (symbol == 'x') node = 1U;
        if (symbol == 'y') node = 2U;
        if (symbol == 'z') node = 3U;
        roots += "\"" + std::string(1U, symbol) + "\":" +
                 std::to_string(node);
    }
    roots.push_back('}');
    return roots;
}

std::string lts_leaf(std::size_t id,
                     std::string_view segment,
                     bool syllable_end) {
    return "{\"emissions\":[{\"segment_ids\":[" +
           std::string(segment) + "],\"syllable_end\":" +
           (syllable_end ? "\"primary\"" : "null") +
           "}],\"id\":" + std::to_string(id) +
           ",\"kind\":\"leaf\",\"schema\":\"kilix.voicegen.lts-node/v1\"}\n";
}

std::string lts_fixture(kgv::PronunciationAdmission admission,
                        std::string_view inventory_sha256,
                        std::string_view lexicon_sha256,
                        std::string_view review) {
    const bool product =
        admission == kgv::PronunciationAdmission::product_admitted;
    std::string resource =
        "{\"admission\":\"" + admission_name(admission) +
        "\",\"context_left\":0,\"context_right\":0,"
        "\"dialect\":\"en-AU\",\"maximum_steps\":1,\"node_count\":4," +
        "\"resource_id\":\"kilix-en-au-resolution-lts-test-1\"," +
        "\"review_record_sha256\":" + review_json(review) +
        ",\"roots\":" + lts_roots(product) +
        ",\"schema\":\"kilix.voicegen.lts-model/v1\"," +
        "\"segment_inventory_sha256\":\"" +
        std::string(inventory_sha256) +
        "\",\"source_lexicon_sha256\":\"" +
        std::string(lexicon_sha256) +
        "\",\"training_record_sha256\":\"" + std::string(64U, '2') +
        "\"}\n";
    resource += lts_leaf(0U, "2", false);
    resource += lts_leaf(1U, "4", false);
    resource += lts_leaf(2U, "1", false);
    resource += lts_leaf(3U, "3", true);
    return resource;
}

std::string token_control(std::size_t id, std::string_view name) {
    return "{\"id\":" + std::to_string(id) +
           ",\"kind\":\"control\",\"name\":\"" + std::string(name) +
           "\",\"schema\":\"kilix.voicegen.model-token-entry/v1\"," +
           "\"segment_id\":null}\n";
}

std::string token_segment(std::size_t id,
                          const kgv::SegmentDefinition &segment) {
    return "{\"id\":" + std::to_string(id) +
           ",\"kind\":\"segment\",\"name\":\"" + segment.name +
           "\",\"schema\":\"kilix.voicegen.model-token-entry/v1\"," +
           "\"segment_id\":" + std::to_string(segment.id) + "}\n";
}

std::string token_fixture(kgv::PronunciationAdmission admission,
                          std::string_view inventory_sha256,
                          std::string_view frontend_abi_sha256,
                          std::size_t budget = 32U) {
    std::string resource =
        "{\"admission\":\"" + admission_name(admission) +
        "\",\"dialect\":\"en-AU\",\"entry_count\":21," +
        "\"frontend_abi_sha256\":\"" +
        std::string(frontend_abi_sha256) +
        "\",\"maximum_input_tokens\":" + std::to_string(budget) +
        ",\"resource_id\":\"kilix-en-au-resolution-tokens-test-1\"," +
        "\"schema\":\"kilix.voicegen.model-token-inventory/v1\"," +
        "\"segment_inventory_sha256\":\"" +
        std::string(inventory_sha256) + "\"}\n";
    for (std::size_t index = 0U; index < controls().size(); ++index) {
        resource += token_control(index, controls()[index]);
    }
    for (std::size_t index = 0U; index < segments().size(); ++index) {
        resource += token_segment(17U + index, segments()[index]);
    }
    return resource;
}

struct LoadedResources final {
    kgv::PronunciationLexicon lexicon;
    kgv::LtsModel lts;
    kgv::ModelTokenInventory tokens;

    kgv::ResolvedFrontendResources chain(
        kgv::PronunciationAdmission admission,
        std::string_view frontend_abi = kFrontendAbi) const {
        return {&lexicon, &lts, &tokens, admission,
                std::string(frontend_abi)};
    }
};

LoadedResources load_resources(
    kgv::PronunciationAdmission admission,
    std::string_view lexicon_review = {},
    std::string_view lts_review = {},
    std::string_view lts_source_override = {},
    std::string_view token_abi = kFrontendAbi) {
    const std::string inventory_sha256 =
        kgv::pronunciation_segment_inventory_sha256(segments());
    require(inventory_sha256.size() == 64U,
            "resolution inventory hash was not produced");
    const std::string lexicon_resource = pronunciation_fixture(
        admission, inventory_sha256, lexicon_review);
    LoadedResources loaded;
    kgv::PronunciationResourceFailure pronunciation_failure;
    require(kgv::load_pronunciation_lexicon(
                lexicon_resource, kgv::sha256_hex(lexicon_resource),
                inventory_sha256, admission, segments(), &loaded.lexicon,
                &pronunciation_failure) == KGV_OK,
            "resolution lexicon fixture did not load");

    const std::string source_hash = lts_source_override.empty()
                                        ? loaded.lexicon.resource_sha256()
                                        : std::string(lts_source_override);
    const std::string lts_resource = lts_fixture(
        admission, inventory_sha256, source_hash, lts_review);
    kgv::LtsFailure lts_failure;
    require(kgv::load_lts_model(
                lts_resource, kgv::sha256_hex(lts_resource), inventory_sha256,
                admission, segments(), &loaded.lts, &lts_failure) == KGV_OK,
            "resolution LTS fixture did not load");

    const std::string token_resource = token_fixture(
        admission, inventory_sha256, token_abi);
    kgv::ModelTokenFailure token_failure;
    require(kgv::load_model_token_inventory(
                token_resource, kgv::sha256_hex(token_resource),
                inventory_sha256, admission, segments(), &loaded.tokens,
                &token_failure) == KGV_OK,
            "resolution token fixture did not load");
    return loaded;
}

void require_cleared(const kgv::ResolvedFrontendResult &result) {
    require(result.profile.empty() && result.input_bytes == 0U &&
                result.frontend_abi_sha256.empty() &&
                result.pronunciation_lexicon_sha256.empty() &&
                result.lts_sha256.empty() && result.words.empty() &&
                result.phrases.empty() && result.diagnostics.empty() &&
                result.model_tokens.inventory_sha256.empty() &&
                result.model_tokens.chunks.empty(),
            "failed resolved frontend retained partial output");
}

void require_failure(std::string_view text,
                     const kgv::ResolvedFrontendResources &resources,
                     const std::vector<std::string> &roles,
                     int expected_status,
                     std::string_view expected_code) {
    kgv::ResolvedFrontendResult result;
    result.profile = "sentinel";
    result.words.push_back({});
    kgv::ResolvedFrontendFailure failure;
    const int status = kgv::run_resolved_frontend(
        text, KGV_PROFILE_PROSE, resources, roles, &result, &failure);
    require(status == expected_status && failure.status == expected_status,
            "resolved frontend failure returned the wrong status");
    if (failure.code != expected_code) {
        throw std::runtime_error("expected resolved failure " +
                                 std::string(expected_code) + ", got " +
                                 failure.code);
    }
    require_cleared(result);
}

void require_same(const kgv::ResolvedFrontendResult &left,
                  const kgv::ResolvedFrontendResult &right) {
    require(left.profile == right.profile &&
                left.input_bytes == right.input_bytes &&
                left.frontend_abi_sha256 == right.frontend_abi_sha256 &&
                left.pronunciation_lexicon_sha256 ==
                    right.pronunciation_lexicon_sha256 &&
                left.lts_sha256 == right.lts_sha256 &&
                left.words.size() == right.words.size() &&
                left.phrases.size() == right.phrases.size() &&
                left.diagnostics.size() == right.diagnostics.size() &&
                kgv::model_token_result_json(left.model_tokens) ==
                    kgv::model_token_result_json(right.model_tokens),
            "resolved frontend repeat changed top-level output");
    for (std::size_t index = 0U; index < left.words.size(); ++index) {
        const kgv::ResolvedFrontendWord &a = left.words[index];
        const kgv::ResolvedFrontendWord &b = right.words[index];
        require(a.normalized == b.normalized &&
                    a.source_kind == b.source_kind && a.role == b.role &&
                    a.pronunciation_source == b.pronunciation_source &&
                    a.span.byte_start == b.span.byte_start &&
                    a.span.byte_end == b.span.byte_end &&
                    a.syllables.size() == b.syllables.size(),
                "resolved frontend repeat changed a word");
        for (std::size_t syllable = 0U; syllable < a.syllables.size();
             ++syllable) {
            require(a.syllables[syllable].stress ==
                        b.syllables[syllable].stress &&
                        a.syllables[syllable].segment_ids ==
                            b.syllables[syllable].segment_ids,
                    "resolved frontend repeat changed a syllable");
        }
    }
    for (std::size_t index = 0U; index < left.phrases.size(); ++index) {
        const kgv::ResolvedTokenPhrase &a = left.phrases[index];
        const kgv::ResolvedTokenPhrase &b = right.phrases[index];
        require(a.word_start == b.word_start && a.word_end == b.word_end &&
                    a.terminator == b.terminator &&
                    a.span.byte_start == b.span.byte_start &&
                    a.span.byte_end == b.span.byte_end,
                "resolved frontend repeat changed a phrase");
    }
    for (std::size_t index = 0U; index < left.diagnostics.size(); ++index) {
        const kgv::FrontendDiagnostic &a = left.diagnostics[index];
        const kgv::FrontendDiagnostic &b = right.diagnostics[index];
        require(a.code == b.code && a.severity == b.severity &&
                    a.span.byte_start == b.span.byte_start &&
                    a.span.byte_end == b.span.byte_end,
                "resolved frontend repeat changed a diagnostic");
    }
}

}  // namespace

int main() {
    try {
        const LoadedResources loaded = load_resources(
            kgv::PronunciationAdmission::test_fixture);
        const kgv::ResolvedFrontendResources resources = loaded.chain(
            kgv::PronunciationAdmission::test_fixture);

        kgv::ResolvedFrontendResult result;
        kgv::ResolvedFrontendFailure failure;
        const std::vector<std::string> noun_roles = {"noun", "default",
                                                     "default"};
        require(kgv::run_resolved_frontend(
                    "Record cat bat.", KGV_PROFILE_PROSE, resources,
                    noun_roles, &result, &failure) == KGV_OK,
                "integrated resolved frontend rejected a valid request");
        require(failure.code.empty() && result.profile == "prose" &&
                    result.input_bytes == 15U && result.words.size() == 3U &&
                    result.phrases.size() == 1U,
                "integrated frontend metadata changed");
        require(result.frontend_abi_sha256 == kFrontendAbi &&
                    result.pronunciation_lexicon_sha256 ==
                        loaded.lexicon.resource_sha256() &&
                    result.lts_sha256 == loaded.lts.resource_sha256() &&
                    result.model_tokens.inventory_sha256 ==
                        loaded.tokens.resource_sha256(),
                "integrated frontend lost resource bindings");
        require(result.words[0U].normalized == "Record" &&
                    result.words[0U].role == "noun" &&
                    result.words[0U].pronunciation_source ==
                        kgv::ResolvedPronunciationSource::base_lexicon &&
                    result.words[0U].syllables[0U].segment_ids ==
                        std::vector<std::uint16_t>({1U}),
                "role-qualified lexicon resolution changed");
        require(result.words[1U].pronunciation_source ==
                    kgv::ResolvedPronunciationSource::base_lexicon &&
                    result.words[2U].pronunciation_source ==
                        kgv::ResolvedPronunciationSource::lts &&
                    result.words[2U].syllables[0U].segment_ids ==
                        std::vector<std::uint16_t>({4U, 2U, 3U}),
                "lexicon/LTS precedence changed");
        require(result.model_tokens.chunks.size() == 1U &&
                    result.model_tokens.chunks[0U].ids ==
                        std::vector<std::uint16_t>({
                            1U, 3U, 4U, 6U, 17U,
                            3U, 4U, 6U, 17U, 18U, 19U,
                            3U, 4U, 6U, 20U, 18U, 19U, 12U, 2U,
                        }) &&
                    result.model_tokens.chunks[0U].source_span.byte_start == 0U &&
                    result.model_tokens.chunks[0U].source_span.byte_end == 15U,
                "end-to-end exact model IDs changed");
        require(std::string(kgv::resolved_pronunciation_source_name(
                    result.words[2U].pronunciation_source)) == "LTS",
                "pronunciation source name changed");

        kgv::ResolvedFrontendResult repeated;
        require(kgv::run_resolved_frontend(
                    "Record cat bat.", KGV_PROFILE_PROSE, resources,
                    noun_roles, &repeated, &failure) == KGV_OK,
                "repeated resolved frontend failed");
        require_same(result, repeated);

        for (std::string_view silent : {std::string_view{},
                                        std::string_view{" \t\r\n"}}) {
            kgv::ResolvedFrontendResult empty;
            require(kgv::run_resolved_frontend(
                        silent, KGV_PROFILE_PROSE, resources, {}, &empty,
                        &failure) == KGV_OK && empty.words.empty() &&
                        empty.phrases.empty() &&
                        empty.model_tokens.chunks.empty() &&
                        empty.model_tokens.inventory_sha256 ==
                            loaded.tokens.resource_sha256(),
                    "silent resolved input did not produce a bound empty result");
        }
        {
            kgv::ResolvedFrontendResult leading_break;
            require(kgv::run_resolved_frontend(
                        "\n\ncat.", KGV_PROFILE_PROSE, resources, {},
                        &leading_break, &failure) == KGV_OK &&
                        leading_break.words.size() == 1U &&
                        leading_break.phrases.size() == 1U &&
                        leading_break.model_tokens.chunks.size() == 1U,
                    "leading empty paragraph broke resolved phrase coverage");
        }

        {
            kgv::ResolvedFrontendResult verb;
            const std::vector<std::string> roles = {"verb", "default",
                                                    "default"};
            require(kgv::run_resolved_frontend(
                        "Record cat bat.", KGV_PROFILE_PROSE, resources,
                        roles, &verb, &failure) == KGV_OK &&
                        verb.words[0U].syllables[0U].segment_ids ==
                            std::vector<std::uint16_t>({4U}) &&
                        verb.model_tokens.chunks[0U].ids[4U] == 20U,
                    "role change did not reach exact model IDs");
        }
        {
            kgv::ResolvedFrontendResult defaulted;
            const std::vector<std::string> roles = {"noun"};
            require(kgv::run_resolved_frontend(
                        "cat.", KGV_PROFILE_PROSE, resources, roles,
                        &defaulted, &failure) == KGV_OK &&
                        defaulted.diagnostics.size() == 1U &&
                        defaulted.diagnostics[0U].code ==
                            "HETERONYM_DEFAULTED",
                    "missing role variant did not surface defaulting");
        }

        require_failure("Record cat bat.", resources, {"noun"},
                        KGV_INVALID_ARGUMENT,
                        "FRONTEND_ROLE_COUNT_MISMATCH");
        require_failure("cat.", resources, {"NotCanonical"},
                        KGV_INVALID_ARGUMENT, "INVALID_FRONTEND_WORD_ROLE");
        require_failure("Record.", resources, {}, KGV_INVALID_TEXT,
                        "AMBIGUOUS_PRONUNCIATION_ROLE");
        require_failure("dad.", resources, {}, KGV_INVALID_TEXT,
                        "UNKNOWN_PRONUNCIATION");
        require_failure("ABC.", resources, {}, KGV_INVALID_TEXT,
                        "UNKNOWN_PRONUNCIATION");

        {
            kgv::ResolvedFrontendResources mismatched = resources;
            mismatched.expected_frontend_abi_sha256 = std::string(64U, 'b');
            require_failure("cat.", mismatched, {}, KGV_ABI_MISMATCH,
                            "FRONTEND_TOKEN_ABI_MISMATCH");
        }
        {
            const LoadedResources wrong_source = load_resources(
                kgv::PronunciationAdmission::test_fixture, {}, {},
                std::string(64U, 'b'));
            require_failure(
                "cat.",
                wrong_source.chain(kgv::PronunciationAdmission::test_fixture),
                {}, KGV_ABI_MISMATCH, "FRONTEND_LTS_LEXICON_MISMATCH");
        }
        {
            kgv::ResolvedFrontendResources mismatched = resources;
            mismatched.required_admission =
                kgv::PronunciationAdmission::product_admitted;
            require_failure("cat.", mismatched, {}, KGV_ABI_MISMATCH,
                            "FRONTEND_RESOURCE_ADMISSION_MISMATCH");
        }
        {
            const std::string review_a(64U, 'a');
            const std::string review_b(64U, 'b');
            const LoadedResources product = load_resources(
                kgv::PronunciationAdmission::product_admitted, review_a,
                review_b);
            require_failure(
                "cat.",
                product.chain(kgv::PronunciationAdmission::product_admitted),
                {}, KGV_ABI_MISMATCH, "FRONTEND_REVIEW_RECORD_MISMATCH");
        }

        std::mt19937_64 generator(0x4b47562d5245534fULL);
        constexpr std::string_view alphabet = "catb .?,";
        std::uniform_int_distribution<std::size_t> length_distribution(0U,
                                                                        96U);
        std::uniform_int_distribution<std::size_t> character_distribution(
            0U, alphabet.size() - 1U);
        for (std::size_t iteration = 0U; iteration < 2000U; ++iteration) {
            std::string input;
            const std::size_t length = length_distribution(generator);
            input.reserve(length);
            for (std::size_t index = 0U; index < length; ++index) {
                input.push_back(alphabet[character_distribution(generator)]);
            }
            kgv::ResolvedFrontendResult first;
            kgv::ResolvedFrontendResult second;
            kgv::ResolvedFrontendFailure first_failure;
            kgv::ResolvedFrontendFailure second_failure;
            const int first_status = kgv::run_resolved_frontend(
                input, KGV_PROFILE_PROSE, resources, {}, &first,
                &first_failure);
            const int second_status = kgv::run_resolved_frontend(
                input, KGV_PROFILE_PROSE, resources, {}, &second,
                &second_failure);
            require(first_status == second_status &&
                        first_failure.code == second_failure.code &&
                        first_failure.span.byte_start ==
                            second_failure.span.byte_start &&
                        first_failure.span.byte_end ==
                            second_failure.span.byte_end &&
                        first_failure.word_index ==
                            second_failure.word_index &&
                        first_failure.has_word == second_failure.has_word,
                    "resolved frontend fuzz result was nondeterministic");
            if (first_status == KGV_OK) {
                require_same(first, second);
            } else {
                require_cleared(first);
                require_cleared(second);
            }
        }

        std::cout << "resolved frontend integration tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
