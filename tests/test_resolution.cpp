#include "frontend/resolution.h"

#include <array>
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
        {"K", 1U},  {"AE", 2U}, {"T", 3U},  {"B", 4U},
        {"S", 5U},  {"Z", 6U},  {"IH", 7U}, {"D", 8U},
        {"NG", 9U}, {"SH", 10U},
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
        "\",\"dialect\":\"en-AU\",\"entry_count\":25," +
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
        "record", "[\"default\"]",
        "[{\"segments\":[\"K\"],\"stress\":\"primary\"}]");
    resource += pronunciation_entry(
        "cat", "[\"default\"]",
        "[{\"segments\":[\"K\",\"AE\",\"T\"],"
        "\"stress\":\"primary\"}]");
    resource += pronunciation_entry(
        "i", "[\"default\"]",
        "[{\"segments\":[\"AE\"],\"stress\":\"primary\"}]");
    resource += pronunciation_entry(
        "will", "[\"default\"]",
        "[{\"segments\":[\"B\"],\"stress\":\"primary\"}]");
    resource += pronunciation_entry(
        "a", "[\"default\"]",
        "[{\"segments\":[\"AE\"],\"stress\":\"none\"}]");
    resource += pronunciation_entry(
        "lead", "[\"noun\"]",
        "[{\"segments\":[\"K\"],\"stress\":\"primary\"}]");
    resource += pronunciation_entry(
        "lead", "[\"verb\"]",
        "[{\"segments\":[\"B\"],\"stress\":\"primary\"}]");
    resource += pronunciation_entry(
        "walk", "[\"default\"]",
        "[{\"segments\":[\"B\",\"AE\",\"K\"],"
        "\"stress\":\"primary\"}]");
    resource += pronunciation_entry(
        "walked", "[\"default\"]",
        "[{\"segments\":[\"B\"],\"stress\":\"primary\"}]");
    resource += pronunciation_entry(
        "ax", "[\"default\"]",
        "[{\"segments\":[\"AE\",\"K\",\"S\"],"
        "\"stress\":\"primary\"}]");
    resource += pronunciation_entry(
        "axe", "[\"default\"]",
        "[{\"segments\":[\"AE\",\"K\",\"Z\"],"
        "\"stress\":\"primary\"}]");
    resource += pronunciation_entry(
        "records", "[\"noun\"]",
        "[{\"segments\":[\"K\",\"Z\"],\"stress\":\"primary\"}]");
    resource += pronunciation_entry(
        "a", "[\"weak\"]",
        "[{\"segments\":[\"IH\"],\"stress\":\"none\"}]");
    resource += pronunciation_entry(
        "an", "[\"default\"]",
        "[{\"segments\":[\"AE\",\"NG\"],\"stress\":\"none\"}]");
    resource += pronunciation_entry(
        "an", "[\"weak\"]",
        "[{\"segments\":[\"IH\",\"NG\"],\"stress\":\"none\"}]");
    resource += pronunciation_entry(
        "hour", "[\"default\"]",
        "[{\"segments\":[\"AE\"],\"stress\":\"primary\"}]");
    resource += pronunciation_entry(
        "one", "[\"default\"]",
        "[{\"segments\":[\"K\"],\"stress\":\"primary\"}]");
    resource += pronunciation_entry(
        "the", "[\"default\"]",
        "[{\"segments\":[\"D\",\"AE\"],\"stress\":\"primary\"}]");
    resource += pronunciation_entry(
        "the", "[\"weak-consonant\"]",
        "[{\"segments\":[\"D\"],\"stress\":\"none\"}]");
    resource += pronunciation_entry(
        "the", "[\"weak-vowel\"]",
        "[{\"segments\":[\"D\",\"IH\"],\"stress\":\"none\"}]");
    resource += pronunciation_entry(
        "to", "[\"default\"]",
        "[{\"segments\":[\"T\",\"AE\"],\"stress\":\"primary\"}]");
    resource += pronunciation_entry(
        "to", "[\"weak-consonant\"]",
        "[{\"segments\":[\"T\"],\"stress\":\"none\"}]");
    resource += pronunciation_entry(
        "to", "[\"weak-vowel\"]",
        "[{\"segments\":[\"T\",\"IH\"],\"stress\":\"none\"}]");
    return resource;
}

std::string morphology_fixture(
    kgv::PronunciationAdmission admission,
    std::string_view base_lexicon_sha256,
    std::string_view inventory_sha256,
    std::string_view review) {
    return "{\"admission\":\"" + admission_name(admission) +
           "\",\"base_lexicon_sha256\":\"" +
           std::string(base_lexicon_sha256) +
           "\",\"dialect\":\"en-AU\","
           "\"past_syllabic_finals\":[\"T\",\"D\"],"
           "\"past_syllabic_suffix\":[\"IH\",\"D\"],"
           "\"past_unvoiced_finals\":[\"K\",\"S\",\"SH\"],"
           "\"past_unvoiced_suffix\":[\"T\"],"
           "\"past_voiced_suffix\":[\"D\"],"
           "\"plural_sibilant_finals\":[\"S\",\"SH\"],"
           "\"plural_sibilant_suffix\":[\"IH\",\"Z\"],"
           "\"plural_unvoiced_finals\":[\"K\",\"T\"],"
           "\"plural_unvoiced_suffix\":[\"S\"],"
           "\"plural_voiced_suffix\":[\"Z\"],"
           "\"progressive_suffix\":[\"IH\",\"NG\"],"
           "\"resource_id\":\"kilix-en-au-resolution-morphology-test-1\","
           "\"review_record_sha256\":" + review_json(review) +
           ",\"schema\":\"kilix.voicegen.morphology-rules/v1\","
           "\"segment_inventory_sha256\":\"" +
           std::string(inventory_sha256) + "\"}\n";
}

std::string heteronym_rule(std::string_view id,
                           std::string_view role,
                           std::string_view capitalization,
                           std::string_view conditions) {
    return "{\"capitalization\":\"" + std::string(capitalization) +
           "\",\"conditions\":" + std::string(conditions) +
           ",\"position\":\"any\",\"role\":\"" + std::string(role) +
           "\",\"rule_id\":\"" + std::string(id) +
           "\",\"schema\":\"kilix.voicegen.heteronym-rule/v1\"," +
           "\"source\":\"project-test-fixture\"," +
           "\"target\":\"record\"}\n";
}

std::string heteronym_fixture(
    kgv::PronunciationAdmission admission,
    std::string_view base_lexicon_sha256,
    std::string_view review,
    bool ambiguous = false) {
    const std::size_t entry_count = ambiguous ? 3U : 2U;
    std::string resource =
        "{\"admission\":\"" + admission_name(admission) +
        "\",\"base_lexicon_sha256\":\"" +
        std::string(base_lexicon_sha256) +
        "\",\"dialect\":\"en-AU\",\"entry_count\":" +
        std::to_string(entry_count) +
        ",\"resource_id\":\"kilix-en-au-resolution-heteronyms-test-1\"," +
        "\"review_record_sha256\":" + review_json(review) +
        ",\"schema\":\"kilix.voicegen.heteronym-rules/v1\"}\n";
    resource += heteronym_rule(
        "fixture.record.after-a", "noun", "any",
        "[{\"offset\":-1,\"words\":[\"a\"]}]");
    resource += heteronym_rule(
        "fixture.record.after-will", "verb", "lower",
        "[{\"offset\":-1,\"words\":[\"will\"]}]");
    if (ambiguous) {
        resource += heteronym_rule("fixture.record.any-lower", "noun",
                                   "lower", "[]");
    }
    return resource;
}

std::string weak_form_rule(std::string_view id,
                           std::string_view target,
                           std::string_view role,
                           std::string_view next_segment) {
    return "{\"capitalization\":\"any\",\"next_segment\":\"" +
           std::string(next_segment) +
           "\",\"position\":\"phrase-medial\",\"role\":\"" +
           std::string(role) + "\",\"rule_id\":\"" + std::string(id) +
           "\",\"schema\":\"kilix.voicegen.weak-form-rule/v1\"," +
           "\"source\":\"project-test-fixture\",\"target\":\"" +
           std::string(target) + "\"}\n";
}

std::string weak_form_fixture(
    kgv::PronunciationAdmission admission,
    std::string_view base_lexicon_sha256,
    std::string_view inventory_sha256,
    std::string_view review,
    bool ambiguous = false) {
    const std::size_t entry_count = ambiguous ? 7U : 6U;
    std::string resource =
        "{\"admission\":\"" + admission_name(admission) +
        "\",\"base_lexicon_sha256\":\"" +
        std::string(base_lexicon_sha256) +
        "\",\"dialect\":\"en-AU\",\"entry_count\":" +
        std::to_string(entry_count) +
        ",\"resource_id\":\"kilix-en-au-resolution-weak-forms-test-1\"," +
        "\"review_record_sha256\":" + review_json(review) +
        ",\"schema\":\"kilix.voicegen.weak-form-rules/v1\"," +
        "\"segment_inventory_sha256\":\"" +
        std::string(inventory_sha256) +
        "\",\"vowel_segments\":[\"AE\",\"IH\"]}\n";
    resource += weak_form_rule("fixture.a.medial", "a", "weak", "any");
    resource += weak_form_rule("fixture.an.medial", "an", "weak", "any");
    resource += weak_form_rule("fixture.the.before-consonant", "the",
                               "weak-consonant", "non-vowel");
    resource += weak_form_rule("fixture.the.before-vowel", "the",
                               "weak-vowel", "vowel");
    if (ambiguous) {
        resource += weak_form_rule("fixture.the.medial", "the",
                                   "weak-consonant", "any");
    }
    resource += weak_form_rule("fixture.to.before-consonant", "to",
                               "weak-consonant", "non-vowel");
    resource += weak_form_rule("fixture.to.before-vowel", "to",
                               "weak-vowel", "vowel");
    return resource;
}

std::string user_dictionary_fixture(std::string_view inventory_sha256) {
    std::string resource =
        "{\"admission\":\"local-user\",\"dialect\":\"en-AU\","
        "\"entry_count\":4,"
        "\"resource_id\":\"kilix-en-au-resolution-user-dictionary-test-1\","
        "\"review_record_sha256\":null,"
        "\"schema\":\"kilix.voicegen.pronunciation-lexicon/v1\","
        "\"segment_inventory_sha256\":\"" +
        std::string(inventory_sha256) + "\"}\n";
    resource += pronunciation_entry(
        "record", "[\"noun\"]",
        "[{\"segments\":[\"T\"],\"stress\":\"primary\"}]");
    resource += pronunciation_entry(
        "cat", "[\"default\"]",
        "[{\"segments\":[\"B\"],\"stress\":\"primary\"}]");
    resource += pronunciation_entry(
        "bat", "[\"noun\"]",
        "[{\"segments\":[\"K\"],\"stress\":\"primary\"}]");
    resource += pronunciation_entry(
        "the", "[\"default\"]",
        "[{\"segments\":[\"B\"],\"stress\":\"primary\"}]");
    return resource;
}

std::string lts_roots(bool product) {
    if (!product) {
        return "{\"a\":0,\"b\":1,\"c\":2,\"e\":0,\"s\":3,\"t\":3,\"x\":1}";
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
        "\",\"dialect\":\"en-AU\",\"entry_count\":" +
        std::to_string(controls().size() + segments().size()) + "," +
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
        const kgv::PronunciationLexicon *user_dictionary = nullptr,
        std::string_view frontend_abi = kFrontendAbi,
        const kgv::HeteronymRules *heteronym_rules = nullptr,
        const kgv::MorphologyRules *morphology_rules = nullptr,
        const kgv::WeakFormRules *weak_form_rules = nullptr) const {
        kgv::ResolvedFrontendResources resources;
        resources.base_lexicon = &lexicon;
        resources.user_dictionary = user_dictionary;
        resources.heteronym_rules = heteronym_rules;
        resources.morphology_rules = morphology_rules;
        resources.weak_form_rules = weak_form_rules;
        resources.lts = &lts;
        resources.model_tokens = &tokens;
        resources.required_admission = admission;
        resources.expected_frontend_abi_sha256 = frontend_abi;
        return resources;
    }
};

kgv::HeteronymRules load_heteronyms(
    kgv::PronunciationAdmission admission,
    std::string_view base_lexicon_sha256,
    std::string_view review = {},
    bool ambiguous = false) {
    const std::string resource = heteronym_fixture(
        admission, base_lexicon_sha256, review, ambiguous);
    kgv::HeteronymRules rules;
    kgv::HeteronymResourceFailure failure;
    require(kgv::load_heteronym_rules(
                resource, kgv::sha256_hex(resource), base_lexicon_sha256,
                admission, &rules, &failure) == KGV_OK,
            "resolution heteronym fixture did not load");
    return rules;
}

kgv::MorphologyRules load_morphology(
    kgv::PronunciationAdmission admission,
    std::string_view base_lexicon_sha256,
    std::string_view review = {}) {
    const std::string inventory_sha256 =
        kgv::pronunciation_segment_inventory_sha256(segments());
    const std::string resource = morphology_fixture(
        admission, base_lexicon_sha256, inventory_sha256, review);
    kgv::MorphologyRules rules;
    kgv::MorphologyResourceFailure failure;
    require(kgv::load_morphology_rules(
                resource, kgv::sha256_hex(resource), base_lexicon_sha256,
                inventory_sha256, admission, segments(), &rules,
                &failure) == KGV_OK,
            "resolution morphology fixture did not load");
    return rules;
}

kgv::WeakFormRules load_weak_forms(
    kgv::PronunciationAdmission admission,
    std::string_view base_lexicon_sha256,
    std::string_view review = {},
    bool ambiguous = false,
    std::string_view inventory_sha256 = {}) {
    const std::string actual_inventory =
        inventory_sha256.empty()
            ? kgv::pronunciation_segment_inventory_sha256(segments())
            : std::string(inventory_sha256);
    const std::string resource = weak_form_fixture(
        admission, base_lexicon_sha256, actual_inventory, review, ambiguous);
    kgv::WeakFormRules rules;
    kgv::WeakFormResourceFailure failure;
    require(kgv::load_weak_form_rules(
                resource, kgv::sha256_hex(resource), base_lexicon_sha256,
                actual_inventory, admission, segments(), &rules,
                &failure) == KGV_OK,
            "resolution weak-form fixture did not load");
    return rules;
}

kgv::PronunciationLexicon load_user_dictionary(
    const std::vector<kgv::SegmentDefinition> &dictionary_segments) {
    const std::string inventory_sha256 =
        kgv::pronunciation_segment_inventory_sha256(dictionary_segments);
    require(inventory_sha256.size() == 64U,
            "user-dictionary inventory hash was not produced");
    const std::string resource = user_dictionary_fixture(inventory_sha256);
    kgv::PronunciationLexicon dictionary;
    kgv::PronunciationResourceFailure failure;
    require(kgv::load_pronunciation_lexicon(
                resource, kgv::sha256_hex(resource), inventory_sha256,
                kgv::PronunciationAdmission::local_user,
                dictionary_segments, &dictionary, &failure) == KGV_OK,
            "resolution user-dictionary fixture did not load");
    return dictionary;
}

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
                result.request_override_count == 0U &&
                result.frontend_abi_sha256.empty() &&
                result.user_dictionary_sha256.empty() &&
                result.heteronym_rules_sha256.empty() &&
                result.morphology_rules_sha256.empty() &&
                result.weak_form_rules_sha256.empty() &&
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

void require_override_failure(
    std::string_view text,
    const kgv::ResolvedFrontendResources &resources,
    const std::vector<std::string> &roles,
    const std::vector<kgv::RequestPronunciationOverride> &overrides,
    int expected_status,
    std::string_view expected_code,
    std::size_t expected_override) {
    kgv::ResolvedFrontendResult result;
    result.profile = "sentinel";
    result.request_override_count = 1U;
    result.words.push_back({});
    kgv::ResolvedFrontendFailure failure;
    const int status = kgv::run_resolved_frontend(
        text, KGV_PROFILE_PROSE, resources, roles, overrides, &result,
        &failure);
    require(status == expected_status && failure.status == expected_status,
            "request override failure returned the wrong status");
    if (failure.code != expected_code) {
        throw std::runtime_error("expected override failure " +
                                 std::string(expected_code) + ", got " +
                                 failure.code);
    }
    require(failure.has_override &&
                failure.override_index == expected_override,
            "request override failure lost its override index");
    require_cleared(result);
}

void require_same(const kgv::ResolvedFrontendResult &left,
                  const kgv::ResolvedFrontendResult &right) {
    require(left.profile == right.profile &&
                left.input_bytes == right.input_bytes &&
                left.request_override_count ==
                    right.request_override_count &&
                left.frontend_abi_sha256 == right.frontend_abi_sha256 &&
                left.user_dictionary_sha256 ==
                    right.user_dictionary_sha256 &&
                left.heteronym_rules_sha256 ==
                    right.heteronym_rules_sha256 &&
                left.morphology_rules_sha256 ==
                    right.morphology_rules_sha256 &&
                left.weak_form_rules_sha256 ==
                    right.weak_form_rules_sha256 &&
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
                    a.role_source == b.role_source &&
                    a.context_rule_id == b.context_rule_id &&
                    a.has_weak_form == b.has_weak_form &&
                    a.weak_form_rule_id == b.weak_form_rule_id &&
                    a.pronunciation_source == b.pronunciation_source &&
                    a.has_morphology == b.has_morphology &&
                    a.morphology_kind == b.morphology_kind &&
                    a.morphology_stem == b.morphology_stem &&
                    a.morphology_stem_source ==
                        b.morphology_stem_source &&
                    a.request_override_kind == b.request_override_kind &&
                    a.request_override_index == b.request_override_index &&
                    a.has_request_override == b.has_request_override &&
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
                    result.input_bytes == 15U &&
                    result.request_override_count == 0U &&
                    result.words.size() == 3U &&
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
                    !result.words[0U].has_request_override &&
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

        {
            const kgv::PronunciationLexicon user_dictionary =
                load_user_dictionary(segments());
            const kgv::ResolvedFrontendResources user_resources =
                loaded.chain(kgv::PronunciationAdmission::test_fixture,
                             &user_dictionary);
            kgv::ResolvedFrontendResult overridden;
            const std::vector<std::string> roles = {"noun", "default"};
            require(kgv::run_resolved_frontend(
                        "Record cat.", KGV_PROFILE_PROSE, user_resources,
                        roles, &overridden, &failure) == KGV_OK,
                    "valid explicit user dictionary was rejected");
            require(overridden.user_dictionary_sha256 ==
                        user_dictionary.resource_sha256() &&
                        overridden.pronunciation_lexicon_sha256 ==
                            loaded.lexicon.resource_sha256() &&
                        overridden.words.size() == 2U &&
                        overridden.words[0U].pronunciation_source ==
                            kgv::ResolvedPronunciationSource::user_dictionary &&
                        overridden.words[1U].pronunciation_source ==
                            kgv::ResolvedPronunciationSource::user_dictionary &&
                        overridden.words[0U].syllables[0U].segment_ids ==
                            std::vector<std::uint16_t>({3U}) &&
                        overridden.words[1U].syllables[0U].segment_ids ==
                            std::vector<std::uint16_t>({4U}),
                    "user dictionary did not precede the base lexicon");
            require(overridden.model_tokens.chunks.size() == 1U &&
                        overridden.model_tokens.chunks[0U].ids ==
                            std::vector<std::uint16_t>({
                                1U, 3U, 4U, 6U, 19U,
                                3U, 4U, 6U, 20U, 12U, 2U,
                            }),
                    "user dictionary did not reach exact model IDs");
            require(std::string(kgv::resolved_pronunciation_source_name(
                        overridden.words[0U].pronunciation_source)) ==
                        "USER_DICTIONARY",
                    "user-dictionary pronunciation source name changed");

            kgv::ResolvedFrontendResult base_variant;
            require(kgv::run_resolved_frontend(
                        "Record.", KGV_PROFILE_PROSE, user_resources,
                        {"verb"}, &base_variant, &failure) == KGV_OK &&
                        base_variant.words.size() == 1U &&
                        base_variant.words[0U].pronunciation_source ==
                            kgv::ResolvedPronunciationSource::base_lexicon &&
                        base_variant.words[0U].syllables[0U].segment_ids ==
                            std::vector<std::uint16_t>({4U}),
                    "missing user role did not defer to the base lexicon");

            kgv::ResolvedFrontendResult defaulted_user;
            require(kgv::run_resolved_frontend(
                        "cat.", KGV_PROFILE_PROSE, user_resources, {"noun"},
                        &defaulted_user, &failure) == KGV_OK &&
                        defaulted_user.words[0U].pronunciation_source ==
                            kgv::ResolvedPronunciationSource::user_dictionary &&
                        defaulted_user.diagnostics.size() == 1U &&
                        defaulted_user.diagnostics[0U].code ==
                            "HETERONYM_DEFAULTED",
                    "user dictionary default did not surface role fallback");

            require_failure("bat.", user_resources, {"verb"},
                            KGV_INVALID_TEXT,
                            "AMBIGUOUS_PRONUNCIATION_ROLE");

            kgv::ResolvedFrontendResult repeated_user;
            require(kgv::run_resolved_frontend(
                        "Record cat.", KGV_PROFILE_PROSE, user_resources,
                        roles, &repeated_user, &failure) == KGV_OK,
                    "repeated user-dictionary resolution failed");
            require_same(overridden, repeated_user);

            kgv::RequestPronunciationOverride phone_override;
            phone_override.span = kgv::SourceSpan{7U, 10U};
            phone_override.kind =
                kgv::RequestOverrideKind::phone_syllables;
            phone_override.syllables = {{
                kgv::SyllableStress::primary,
                std::vector<std::uint16_t>({2U}),
            }};
            const std::vector<kgv::RequestPronunciationOverride>
                phone_overrides = {phone_override};
            kgv::ResolvedFrontendResult request_overridden;
            require(kgv::run_resolved_frontend(
                        "Record cat.", KGV_PROFILE_PROSE, user_resources,
                        roles, phone_overrides, &request_overridden,
                        &failure) == KGV_OK,
                    "valid typed phone override was rejected");
            require(request_overridden.request_override_count == 1U &&
                        request_overridden.words.size() == 2U &&
                        request_overridden.words[0U].pronunciation_source ==
                            kgv::ResolvedPronunciationSource::user_dictionary &&
                        request_overridden.words[1U].pronunciation_source ==
                            kgv::ResolvedPronunciationSource::request_override &&
                        request_overridden.words[1U].has_request_override &&
                        request_overridden.words[1U].request_override_kind ==
                            kgv::RequestOverrideKind::phone_syllables &&
                        request_overridden.words[1U].request_override_index ==
                            0U &&
                        request_overridden.words[1U]
                                .syllables[0U].segment_ids ==
                            std::vector<std::uint16_t>({2U}),
                    "request override did not precede the user dictionary");
            require(request_overridden.model_tokens.chunks.size() == 1U &&
                        request_overridden.model_tokens.chunks[0U].ids ==
                            std::vector<std::uint16_t>({
                                1U, 3U, 4U, 6U, 19U,
                                3U, 4U, 6U, 18U, 12U, 2U,
                            }) &&
                        std::string(
                            kgv::resolved_pronunciation_source_name(
                                request_overridden.words[1U]
                                    .pronunciation_source)) ==
                            "REQUEST_OVERRIDE",
                    "typed phone override did not reach exact model IDs");

            kgv::ResolvedFrontendResult repeated_request;
            require(kgv::run_resolved_frontend(
                        "Record cat.", KGV_PROFILE_PROSE, user_resources,
                        roles, phone_overrides, &repeated_request,
                        &failure) == KGV_OK,
                    "repeated typed phone override failed");
            require_same(request_overridden, repeated_request);
        }

        {
            const kgv::MorphologyRules morphology_rules = load_morphology(
                kgv::PronunciationAdmission::test_fixture,
                loaded.lexicon.resource_sha256());
            const kgv::ResolvedFrontendResources morphology_resources =
                loaded.chain(kgv::PronunciationAdmission::test_fixture,
                             nullptr, kFrontendAbi, nullptr,
                             &morphology_rules);
            kgv::ResolvedFrontendResult inflected;
            const int morphology_status = kgv::run_resolved_frontend(
                "cats walking walked axes.", KGV_PROFILE_PROSE,
                morphology_resources, {}, &inflected, &failure);
            if (morphology_status != KGV_OK) {
                throw std::runtime_error(
                    "valid productive morphology request was rejected: " +
                    failure.code);
            }
            require(inflected.morphology_rules_sha256 ==
                        morphology_rules.resource_sha256() &&
                        inflected.words.size() == 4U &&
                        inflected.words[0U].pronunciation_source ==
                            kgv::ResolvedPronunciationSource::morphology &&
                        inflected.words[0U].has_morphology &&
                        inflected.words[0U].morphology_kind ==
                            kgv::MorphologyKind::plural_or_possessive &&
                        inflected.words[0U].morphology_stem == "cat" &&
                        inflected.words[0U].morphology_stem_source ==
                            kgv::ResolvedPronunciationSource::base_lexicon &&
                        inflected.words[0U]
                                .syllables[0U].segment_ids ==
                            std::vector<std::uint16_t>({1U, 2U, 3U, 5U}),
                    "plural morphology lost output or provenance");
            require(inflected.words[1U].pronunciation_source ==
                        kgv::ResolvedPronunciationSource::morphology &&
                        inflected.words[1U].morphology_kind ==
                            kgv::MorphologyKind::progressive &&
                        inflected.words[1U].morphology_stem == "walk" &&
                        inflected.words[1U].syllables.size() == 2U &&
                        inflected.words[1U]
                                .syllables[1U].stress ==
                            kgv::SyllableStress::none &&
                        inflected.words[1U]
                                .syllables[1U].segment_ids ==
                            std::vector<std::uint16_t>({7U, 9U}),
                    "progressive morphology lost syllabification");
            require(inflected.words[2U].pronunciation_source ==
                        kgv::ResolvedPronunciationSource::base_lexicon &&
                        !inflected.words[2U].has_morphology &&
                        inflected.words[2U]
                                .syllables[0U].segment_ids ==
                            std::vector<std::uint16_t>({4U}),
                    "whole-word lexicon did not precede morphology");
            require(inflected.words[3U].pronunciation_source ==
                        kgv::ResolvedPronunciationSource::lts &&
                        !inflected.words[3U].has_morphology &&
                        inflected.diagnostics.size() == 1U &&
                        inflected.diagnostics[0U].code ==
                            "MORPHOLOGY_AMBIGUOUS" &&
                        std::string(kgv::resolved_pronunciation_source_name(
                            inflected.words[0U].pronunciation_source)) ==
                            "MORPHOLOGY",
                    "ambiguous morphology did not fail closed to LTS");

            const kgv::PronunciationLexicon user_dictionary =
                load_user_dictionary(segments());
            const kgv::ResolvedFrontendResources user_morphology_resources =
                loaded.chain(kgv::PronunciationAdmission::test_fixture,
                             &user_dictionary, kFrontendAbi, nullptr,
                             &morphology_rules);
            kgv::ResolvedFrontendResult user_inflected;
            require(kgv::run_resolved_frontend(
                        "cats.", KGV_PROFILE_PROSE,
                        user_morphology_resources, {}, &user_inflected,
                        &failure) == KGV_OK &&
                        user_inflected.words.size() == 1U &&
                        user_inflected.words[0U].has_morphology &&
                        user_inflected.words[0U].morphology_stem_source ==
                            kgv::ResolvedPronunciationSource::user_dictionary &&
                        user_inflected.words[0U]
                                .syllables[0U].segment_ids ==
                            std::vector<std::uint16_t>({4U, 6U}),
                    "user-dictionary stem did not precede base morphology");

            kgv::RequestPronunciationOverride phone_override;
            phone_override.span = kgv::SourceSpan{0U, 4U};
            phone_override.kind =
                kgv::RequestOverrideKind::phone_syllables;
            phone_override.syllables = {{
                kgv::SyllableStress::primary,
                std::vector<std::uint16_t>({2U}),
            }};
            kgv::ResolvedFrontendResult phone_wins;
            require(kgv::run_resolved_frontend(
                        "cats.", KGV_PROFILE_PROSE,
                        user_morphology_resources, {}, {phone_override},
                        &phone_wins, &failure) == KGV_OK &&
                        phone_wins.words.size() == 1U &&
                        phone_wins.words[0U].pronunciation_source ==
                            kgv::ResolvedPronunciationSource::request_override &&
                        !phone_wins.words[0U].has_morphology,
                    "phone override did not precede morphology");

            kgv::RequestPronunciationOverride replacement_override;
            replacement_override.span = kgv::SourceSpan{0U, 3U};
            replacement_override.kind =
                kgv::RequestOverrideKind::replacement_text;
            replacement_override.replacement_text = "cats";
            kgv::ResolvedFrontendResult replacement_reentered;
            require(kgv::run_resolved_frontend(
                        "cat.", KGV_PROFILE_PROSE, morphology_resources,
                        {}, {replacement_override}, &replacement_reentered,
                        &failure) == KGV_OK &&
                        replacement_reentered.words.size() == 1U &&
                        replacement_reentered.words[0U].normalized == "cats" &&
                        replacement_reentered.words[0U].has_request_override &&
                        replacement_reentered.words[0U].has_morphology &&
                        replacement_reentered.words[0U].span.byte_start == 0U &&
                        replacement_reentered.words[0U].span.byte_end == 3U,
                    "replacement text did not re-enter morphology");

            require_failure("records.", morphology_resources, {"verb"},
                            KGV_INVALID_TEXT,
                            "AMBIGUOUS_PRONUNCIATION_ROLE");

            kgv::ResolvedFrontendResult repeated_morphology;
            require(kgv::run_resolved_frontend(
                        "cats walking walked axes.", KGV_PROFILE_PROSE,
                        morphology_resources, {}, &repeated_morphology,
                        &failure) == KGV_OK,
                    "repeated morphology resolution failed");
            require_same(inflected, repeated_morphology);
        }

        {
            const kgv::HeteronymRules context_rules = load_heteronyms(
                kgv::PronunciationAdmission::test_fixture,
                loaded.lexicon.resource_sha256());
            const kgv::ResolvedFrontendResources context_resources =
                loaded.chain(kgv::PronunciationAdmission::test_fixture,
                             nullptr, kFrontendAbi, &context_rules);
            kgv::ResolvedFrontendResult contextual;
            require(kgv::run_resolved_frontend(
                        "I will record a record.", KGV_PROFILE_PROSE,
                        context_resources, {}, &contextual,
                        &failure) == KGV_OK,
                    "valid contextual heteronym sentence was rejected");
            require(contextual.heteronym_rules_sha256 ==
                        context_rules.resource_sha256() &&
                        contextual.words.size() == 5U &&
                        contextual.diagnostics.empty() &&
                        contextual.words[2U].role == "verb" &&
                        contextual.words[2U].role_source ==
                            kgv::ResolvedRoleSource::contextual_rule &&
                        contextual.words[2U].context_rule_id ==
                            "fixture.record.after-will" &&
                        contextual.words[2U].syllables[0U].segment_ids ==
                            std::vector<std::uint16_t>({4U}) &&
                        contextual.words[4U].role == "noun" &&
                        contextual.words[4U].role_source ==
                            kgv::ResolvedRoleSource::contextual_rule &&
                        contextual.words[4U].context_rule_id ==
                            "fixture.record.after-a" &&
                        contextual.words[4U].syllables[0U].segment_ids ==
                            std::vector<std::uint16_t>({1U}) &&
                        contextual.model_tokens.chunks.size() == 1U &&
                        contextual.model_tokens.chunks[0U].ids ==
                            std::vector<std::uint16_t>({
                                1U,
                                3U, 4U, 6U, 18U,
                                3U, 4U, 6U, 20U,
                                3U, 4U, 6U, 20U,
                                3U, 4U, 5U, 18U,
                                3U, 4U, 6U, 17U,
                                12U, 2U,
                            }),
                    "contextual rules did not select exact heteronym variants");
            require(std::string(kgv::resolved_role_source_name(
                        contextual.words[2U].role_source)) ==
                        "CONTEXTUAL_RULE",
                    "contextual role source name changed");

            kgv::ResolvedFrontendResult defaulted_context;
            require(kgv::run_resolved_frontend(
                        "record.", KGV_PROFILE_PROSE, context_resources, {},
                        &defaulted_context, &failure) == KGV_OK &&
                        defaulted_context.words.size() == 1U &&
                        defaulted_context.words[0U].role == "default" &&
                        defaulted_context.words[0U].role_source ==
                            kgv::ResolvedRoleSource::default_role &&
                        defaulted_context.words[0U].context_rule_id.empty() &&
                        defaulted_context.words[0U]
                                .syllables[0U].segment_ids ==
                            std::vector<std::uint16_t>({1U}) &&
                        defaulted_context.diagnostics.size() == 1U &&
                        defaulted_context.diagnostics[0U].code ==
                            "HETERONYM_DEFAULTED",
                    "unmatched contextual target did not use its documented default");

            kgv::ResolvedFrontendResult explicit_role;
            require(kgv::run_resolved_frontend(
                        "I will record.", KGV_PROFILE_PROSE,
                        context_resources, {"", "", "noun"},
                        &explicit_role, &failure) == KGV_OK &&
                        explicit_role.words[2U].role == "noun" &&
                        explicit_role.words[2U].role_source ==
                            kgv::ResolvedRoleSource::explicit_request &&
                        explicit_role.words[2U].context_rule_id.empty() &&
                        explicit_role.words[2U]
                                .syllables[0U].segment_ids ==
                            std::vector<std::uint16_t>({1U}) &&
                        explicit_role.diagnostics.empty(),
                    "explicit role did not precede contextual selection");

            kgv::RequestPronunciationOverride phone;
            phone.span = kgv::SourceSpan{7U, 13U};
            phone.kind = kgv::RequestOverrideKind::phone_syllables;
            phone.syllables = {{
                kgv::SyllableStress::primary,
                std::vector<std::uint16_t>({2U}),
            }};
            kgv::ResolvedFrontendResult phone_wins;
            require(kgv::run_resolved_frontend(
                        "I will record.", KGV_PROFILE_PROSE,
                        context_resources, {}, {phone}, &phone_wins,
                        &failure) == KGV_OK &&
                        phone_wins.words[2U].pronunciation_source ==
                            kgv::ResolvedPronunciationSource::request_override &&
                        phone_wins.words[2U].role_source ==
                            kgv::ResolvedRoleSource::default_role &&
                        phone_wins.words[2U].context_rule_id.empty() &&
                        phone_wins.words[2U].syllables[0U].segment_ids ==
                            std::vector<std::uint16_t>({2U}) &&
                        phone_wins.diagnostics.empty(),
                    "phone override did not precede contextual selection");

            kgv::RequestPronunciationOverride replacement;
            replacement.span = kgv::SourceSpan{7U, 10U};
            replacement.replacement_text = "record";
            kgv::ResolvedFrontendResult replaced_context;
            require(kgv::run_resolved_frontend(
                        "I will dog.", KGV_PROFILE_PROSE,
                        context_resources, {}, {replacement},
                        &replaced_context, &failure) == KGV_OK &&
                        replaced_context.words[2U].normalized == "record" &&
                        replaced_context.words[2U].span.byte_start == 7U &&
                        replaced_context.words[2U].span.byte_end == 10U &&
                        replaced_context.words[2U].role == "verb" &&
                        replaced_context.words[2U].role_source ==
                            kgv::ResolvedRoleSource::contextual_rule &&
                        replaced_context.words[2U].context_rule_id ==
                            "fixture.record.after-will" &&
                        replaced_context.words[2U].pronunciation_source ==
                            kgv::ResolvedPronunciationSource::base_lexicon,
                    "replacement text did not re-enter contextual role selection");

            const kgv::HeteronymRules ambiguous_rules = load_heteronyms(
                kgv::PronunciationAdmission::test_fixture,
                loaded.lexicon.resource_sha256(), {}, true);
            const kgv::ResolvedFrontendResources ambiguous_resources =
                loaded.chain(kgv::PronunciationAdmission::test_fixture,
                             nullptr, kFrontendAbi, &ambiguous_rules);
            kgv::ResolvedFrontendResult ambiguous_context;
            require(kgv::run_resolved_frontend(
                        "I will record.", KGV_PROFILE_PROSE,
                        ambiguous_resources, {}, &ambiguous_context,
                        &failure) == KGV_OK &&
                        ambiguous_context.words[2U].role == "default" &&
                        ambiguous_context.words[2U].role_source ==
                            kgv::ResolvedRoleSource::default_role &&
                        ambiguous_context.words[2U]
                                .syllables[0U].segment_ids ==
                            std::vector<std::uint16_t>({1U}) &&
                        ambiguous_context.diagnostics.size() == 2U &&
                        ambiguous_context.diagnostics[0U].code ==
                            "HETERONYM_RULE_AMBIGUOUS" &&
                        ambiguous_context.diagnostics[1U].code ==
                            "HETERONYM_DEFAULTED",
                    "overlapping contextual rules did not fail closed to the default");

            kgv::ResolvedFrontendResult repeated_context;
            require(kgv::run_resolved_frontend(
                        "I will record a record.", KGV_PROFILE_PROSE,
                        context_resources, {}, &repeated_context,
                        &failure) == KGV_OK,
                    "repeated contextual request failed");
            require_same(contextual, repeated_context);
        }

        {
            const kgv::WeakFormRules weak_form_rules = load_weak_forms(
                kgv::PronunciationAdmission::test_fixture,
                loaded.lexicon.resource_sha256());
            const kgv::ResolvedFrontendResources weak_form_resources =
                loaded.chain(kgv::PronunciationAdmission::test_fixture,
                             nullptr, kFrontendAbi, nullptr, nullptr,
                             &weak_form_rules);
            constexpr std::string_view weak_form_text =
                "the hour the one to hour to cat a cat an cat the.";
            kgv::ResolvedFrontendResult selected;
            require(kgv::run_resolved_frontend(
                        weak_form_text, KGV_PROFILE_PROSE,
                        weak_form_resources, {}, &selected,
                        &failure) == KGV_OK &&
                        selected.weak_form_rules_sha256 ==
                            weak_form_rules.resource_sha256() &&
                        selected.words.size() == 13U &&
                        selected.diagnostics.empty(),
                    "valid weak-form request was rejected");
            require(selected.words[0U].has_weak_form &&
                        selected.words[0U].role == "weak-vowel" &&
                        selected.words[0U].role_source ==
                            kgv::ResolvedRoleSource::postlexical_rule &&
                        selected.words[0U].weak_form_rule_id ==
                            "fixture.the.before-vowel" &&
                        selected.words[0U]
                                .syllables[0U].segment_ids ==
                            std::vector<std::uint16_t>({8U, 7U}) &&
                        selected.words[2U].role == "weak-consonant" &&
                        selected.words[2U].weak_form_rule_id ==
                            "fixture.the.before-consonant" &&
                        selected.words[2U]
                                .syllables[0U].segment_ids ==
                            std::vector<std::uint16_t>({8U}),
                    "the did not inspect the next pronounced segment");
            require(selected.words[4U].role == "weak-vowel" &&
                        selected.words[4U]
                                .syllables[0U].segment_ids ==
                            std::vector<std::uint16_t>({3U, 7U}) &&
                        selected.words[6U].role == "weak-consonant" &&
                        selected.words[6U]
                                .syllables[0U].segment_ids ==
                            std::vector<std::uint16_t>({3U}) &&
                        selected.words[8U].role == "weak" &&
                        selected.words[8U]
                                .syllables[0U].segment_ids ==
                            std::vector<std::uint16_t>({7U}) &&
                        selected.words[10U].role == "weak" &&
                        selected.words[10U]
                                .syllables[0U].segment_ids ==
                            std::vector<std::uint16_t>({7U, 9U}),
                    "documented function-word weak forms changed");
            require(!selected.words[12U].has_weak_form &&
                        selected.words[12U].role == "default" &&
                        selected.words[12U].role_source ==
                            kgv::ResolvedRoleSource::default_role &&
                        selected.words[12U].weak_form_rule_id.empty() &&
                        selected.words[12U]
                                .syllables[0U].segment_ids ==
                            std::vector<std::uint16_t>({8U, 2U}) &&
                        std::string(kgv::resolved_role_source_name(
                            selected.words[0U].role_source)) ==
                            "POSTLEXICAL_RULE",
                    "phrase-final strong form or weak-form provenance changed");

            kgv::ResolvedFrontendResult across_boundary;
            require(kgv::run_resolved_frontend(
                        "the. hour.", KGV_PROFILE_PROSE,
                        weak_form_resources, {}, &across_boundary,
                        &failure) == KGV_OK &&
                        across_boundary.words.size() == 2U &&
                        !across_boundary.words[0U].has_weak_form &&
                        across_boundary.words[0U]
                                .syllables[0U].segment_ids ==
                            std::vector<std::uint16_t>({8U, 2U}),
                    "weak-form lookahead crossed a phrase boundary");

            kgv::ResolvedFrontendResult explicit_role;
            require(kgv::run_resolved_frontend(
                        "the hour.", KGV_PROFILE_PROSE,
                        weak_form_resources, {"default", ""},
                        &explicit_role, &failure) == KGV_OK &&
                        explicit_role.words[0U].role_source ==
                            kgv::ResolvedRoleSource::explicit_request &&
                        !explicit_role.words[0U].has_weak_form &&
                        explicit_role.words[0U]
                                .syllables[0U].segment_ids ==
                            std::vector<std::uint16_t>({8U, 2U}),
                    "explicit role did not precede weak-form selection");

            kgv::RequestPronunciationOverride target_phone;
            target_phone.span = kgv::SourceSpan{0U, 3U};
            target_phone.kind =
                kgv::RequestOverrideKind::phone_syllables;
            target_phone.syllables = {{
                kgv::SyllableStress::primary,
                std::vector<std::uint16_t>({5U}),
            }};
            kgv::ResolvedFrontendResult phone_wins;
            require(kgv::run_resolved_frontend(
                        "the hour.", KGV_PROFILE_PROSE,
                        weak_form_resources, {}, {target_phone},
                        &phone_wins, &failure) == KGV_OK &&
                        phone_wins.words[0U].pronunciation_source ==
                            kgv::ResolvedPronunciationSource::request_override &&
                        !phone_wins.words[0U].has_weak_form &&
                        phone_wins.words[0U]
                                .syllables[0U].segment_ids ==
                            std::vector<std::uint16_t>({5U}),
                    "phone override did not precede weak-form selection");

            const kgv::PronunciationLexicon user_dictionary =
                load_user_dictionary(segments());
            const kgv::ResolvedFrontendResources user_weak_forms =
                loaded.chain(kgv::PronunciationAdmission::test_fixture,
                             &user_dictionary, kFrontendAbi, nullptr, nullptr,
                             &weak_form_rules);
            kgv::ResolvedFrontendResult user_wins;
            require(kgv::run_resolved_frontend(
                        "the hour.", KGV_PROFILE_PROSE, user_weak_forms, {},
                        &user_wins, &failure) == KGV_OK &&
                        user_wins.words[0U].pronunciation_source ==
                            kgv::ResolvedPronunciationSource::user_dictionary &&
                        !user_wins.words[0U].has_weak_form &&
                        user_wins.words[0U]
                                .syllables[0U].segment_ids ==
                            std::vector<std::uint16_t>({4U}),
                    "user dictionary did not precede weak-form selection");

            kgv::RequestPronunciationOverride next_phone;
            next_phone.span = kgv::SourceSpan{4U, 8U};
            next_phone.kind = kgv::RequestOverrideKind::phone_syllables;
            next_phone.syllables = {{
                kgv::SyllableStress::primary,
                std::vector<std::uint16_t>({1U}),
            }};
            kgv::ResolvedFrontendResult pronounced_context;
            require(kgv::run_resolved_frontend(
                        "the hour.", KGV_PROFILE_PROSE,
                        weak_form_resources, {}, {next_phone},
                        &pronounced_context, &failure) == KGV_OK &&
                        pronounced_context.words[0U].role ==
                            "weak-consonant" &&
                        pronounced_context.words[0U]
                                .syllables[0U].segment_ids ==
                            std::vector<std::uint16_t>({8U}),
                    "weak form inspected spelling instead of overridden phones");

            kgv::RequestPronunciationOverride replacement;
            replacement.span = kgv::SourceSpan{0U, 3U};
            replacement.kind =
                kgv::RequestOverrideKind::replacement_text;
            replacement.replacement_text = "the";
            kgv::ResolvedFrontendResult replaced;
            require(kgv::run_resolved_frontend(
                        "cat hour.", KGV_PROFILE_PROSE,
                        weak_form_resources, {}, {replacement}, &replaced,
                        &failure) == KGV_OK &&
                        replaced.words[0U].normalized == "the" &&
                        replaced.words[0U].has_request_override &&
                        replaced.words[0U].has_weak_form &&
                        replaced.words[0U].role == "weak-vowel",
                    "replacement text did not re-enter weak-form selection");

            const kgv::WeakFormRules ambiguous_rules = load_weak_forms(
                kgv::PronunciationAdmission::test_fixture,
                loaded.lexicon.resource_sha256(), {}, true);
            const kgv::ResolvedFrontendResources ambiguous_resources =
                loaded.chain(kgv::PronunciationAdmission::test_fixture,
                             nullptr, kFrontendAbi, nullptr, nullptr,
                             &ambiguous_rules);
            kgv::ResolvedFrontendResult ambiguous;
            require(kgv::run_resolved_frontend(
                        "the hour.", KGV_PROFILE_PROSE,
                        ambiguous_resources, {}, &ambiguous,
                        &failure) == KGV_OK &&
                        ambiguous.words[0U].role == "default" &&
                        !ambiguous.words[0U].has_weak_form &&
                        ambiguous.words[0U]
                                .syllables[0U].segment_ids ==
                            std::vector<std::uint16_t>({8U, 2U}) &&
                        ambiguous.diagnostics.size() == 1U &&
                        ambiguous.diagnostics[0U].code ==
                            "WEAK_FORM_RULE_AMBIGUOUS",
                    "overlapping weak-form rules did not fail closed");
            kgv::ResolvedFrontendResult ordered_diagnostics;
            require(kgv::run_resolved_frontend(
                        "the hour the hour.", KGV_PROFILE_PROSE,
                        ambiguous_resources, {}, &ordered_diagnostics,
                        &failure) == KGV_OK &&
                        ordered_diagnostics.diagnostics.size() == 2U &&
                        ordered_diagnostics.diagnostics[0U].span.byte_start ==
                            0U &&
                        ordered_diagnostics.diagnostics[1U].span.byte_start ==
                            9U,
                    "right-to-left weak-form selection reordered diagnostics");

            kgv::ResolvedFrontendResult repeated_weak_forms;
            require(kgv::run_resolved_frontend(
                        weak_form_text, KGV_PROFILE_PROSE,
                        weak_form_resources, {}, &repeated_weak_forms,
                        &failure) == KGV_OK,
                    "repeated weak-form request failed");
            require_same(selected, repeated_weak_forms);
        }

        {
            kgv::RequestPronunciationOverride replacement;
            replacement.span = kgv::SourceSpan{0U, 3U};
            replacement.kind = kgv::RequestOverrideKind::replacement_text;
            replacement.replacement_text = "cat";
            kgv::ResolvedFrontendResult replaced;
            require(kgv::run_resolved_frontend(
                        "dog.", KGV_PROFILE_PROSE, resources, {},
                        {replacement}, &replaced, &failure) == KGV_OK,
                    "valid replacement-text override was rejected");
            require(replaced.input_bytes == 4U &&
                        replaced.request_override_count == 1U &&
                        replaced.words.size() == 1U &&
                        replaced.words[0U].normalized == "cat" &&
                        replaced.words[0U].span.byte_start == 0U &&
                        replaced.words[0U].span.byte_end == 3U &&
                        replaced.words[0U].has_request_override &&
                        replaced.words[0U].request_override_kind ==
                            kgv::RequestOverrideKind::replacement_text &&
                        replaced.words[0U].request_override_index == 0U &&
                        replaced.words[0U].pronunciation_source ==
                            kgv::ResolvedPronunciationSource::base_lexicon,
                    "replacement text did not re-enter lexical resolution");

            replacement.span = kgv::SourceSpan{0U, 1U};
            replacement.replacement_text = "Record cat";
            kgv::ResolvedFrontendResult expanded_replacement;
            require(kgv::run_resolved_frontend(
                        "X.", KGV_PROFILE_PROSE, resources,
                        {"noun", "default"}, {replacement},
                        &expanded_replacement, &failure) == KGV_OK,
                    "multiword replacement-text override was rejected");
            require(expanded_replacement.input_bytes == 2U &&
                        expanded_replacement.words.size() == 2U &&
                        expanded_replacement.words[0U].normalized == "Record" &&
                        expanded_replacement.words[1U].normalized == "cat" &&
                        expanded_replacement.words[0U].span.byte_start == 0U &&
                        expanded_replacement.words[0U].span.byte_end == 1U &&
                        expanded_replacement.words[1U].span.byte_start == 0U &&
                        expanded_replacement.words[1U].span.byte_end == 1U &&
                        expanded_replacement.words[0U].has_request_override &&
                        expanded_replacement.words[1U].has_request_override &&
                        expanded_replacement.phrases.size() == 1U &&
                        expanded_replacement.phrases[0U].span.byte_start == 0U &&
                        expanded_replacement.phrases[0U].span.byte_end == 2U,
                    "multiword replacement lost original source provenance");

            kgv::RequestPronunciationOverride shifted_phone;
            shifted_phone.span = kgv::SourceSpan{4U, 7U};
            shifted_phone.kind =
                kgv::RequestOverrideKind::phone_syllables;
            shifted_phone.syllables = {{
                kgv::SyllableStress::primary,
                std::vector<std::uint16_t>({2U}),
            }};
            replacement.span = kgv::SourceSpan{0U, 3U};
            replacement.replacement_text = "Record";
            kgv::ResolvedFrontendResult shifted;
            require(kgv::run_resolved_frontend(
                        "dog cat.", KGV_PROFILE_PROSE, resources,
                        {"noun", "default"},
                        {replacement, shifted_phone}, &shifted,
                        &failure) == KGV_OK,
                    "replacement before a phone override was rejected");
            require(shifted.request_override_count == 2U &&
                        shifted.words.size() == 2U &&
                        shifted.words[0U].normalized == "Record" &&
                        shifted.words[0U].span.byte_start == 0U &&
                        shifted.words[0U].span.byte_end == 3U &&
                        shifted.words[0U].request_override_index == 0U &&
                        shifted.words[1U].normalized == "cat" &&
                        shifted.words[1U].span.byte_start == 4U &&
                        shifted.words[1U].span.byte_end == 7U &&
                        shifted.words[1U].request_override_index == 1U &&
                        shifted.words[1U].pronunciation_source ==
                            kgv::ResolvedPronunciationSource::request_override &&
                        shifted.words[1U].syllables[0U].segment_ids ==
                            std::vector<std::uint16_t>({2U}),
                    "override remapping drifted after a length-changing replacement");
        }

        {
            kgv::RequestPronunciationOverride first;
            first.span = kgv::SourceSpan{0U, 2U};
            first.replacement_text = "cat";
            kgv::RequestPronunciationOverride second;
            second.span = kgv::SourceSpan{1U, 3U};
            second.replacement_text = "cat";
            require_override_failure(
                "cat.", resources, {}, {first, second}, KGV_INVALID_TEXT,
                "OVERLAPPING_OVERRIDE", 1U);
        }
        {
            kgv::RequestPronunciationOverride split_utf8;
            split_utf8.span = kgv::SourceSpan{4U, 5U};
            split_utf8.replacement_text = "cat";
            require_override_failure(
                "caf\xc3\xa9.", resources, {}, {split_utf8},
                KGV_INVALID_TEXT, "INVALID_OVERRIDE_BOUNDARY", 0U);
        }
        {
            kgv::RequestPronunciationOverride split_grapheme;
            split_grapheme.span = kgv::SourceSpan{0U, 1U};
            split_grapheme.replacement_text = "cat";
            require_override_failure(
                "e\xcc\x81.", resources, {}, {split_grapheme},
                KGV_INVALID_TEXT, "INVALID_OVERRIDE_BOUNDARY", 0U);
        }
        {
            kgv::RequestPronunciationOverride unknown_segment;
            unknown_segment.span = kgv::SourceSpan{0U, 3U};
            unknown_segment.kind =
                kgv::RequestOverrideKind::phone_syllables;
            unknown_segment.syllables = {{
                kgv::SyllableStress::primary,
                std::vector<std::uint16_t>({99U}),
            }};
            require_override_failure(
                "cat.", resources, {}, {unknown_segment}, KGV_INVALID_TEXT,
                "UNKNOWN_OVERRIDE_SEGMENT", 0U);
        }
        {
            kgv::RequestPronunciationOverride nonword_phone;
            nonword_phone.span = kgv::SourceSpan{0U, 4U};
            nonword_phone.kind =
                kgv::RequestOverrideKind::phone_syllables;
            nonword_phone.syllables = {{
                kgv::SyllableStress::primary,
                std::vector<std::uint16_t>({1U}),
            }};
            require_override_failure(
                "cat.", resources, {}, {nonword_phone}, KGV_INVALID_TEXT,
                "INVALID_PHONE_OVERRIDE_TARGET", 0U);
        }
        {
            kgv::RequestPronunciationOverride expanded_phone;
            expanded_phone.span = kgv::SourceSpan{0U, 2U};
            expanded_phone.kind =
                kgv::RequestOverrideKind::phone_syllables;
            expanded_phone.syllables = {{
                kgv::SyllableStress::primary,
                std::vector<std::uint16_t>({1U}),
            }};
            require_override_failure(
                "21.", resources, {}, {expanded_phone}, KGV_INVALID_TEXT,
                "INVALID_PHONE_OVERRIDE_TARGET", 0U);
        }
        {
            kgv::RequestPronunciationOverride invalid_replacement;
            invalid_replacement.span = kgv::SourceSpan{4U, 7U};
            invalid_replacement.replacement_text.assign(1U,
                                                        static_cast<char>(0xff));
            require_override_failure(
                "cat dog.", resources, {}, {invalid_replacement},
                KGV_INVALID_TEXT, "INVALID_UTF8", 0U);
        }
        {
            kgv::RequestPronunciationOverride ambiguous_replacement;
            ambiguous_replacement.span = kgv::SourceSpan{0U, 3U};
            ambiguous_replacement.replacement_text = "lead";
            require_override_failure(
                "dog.", resources, {}, {ambiguous_replacement},
                KGV_INVALID_TEXT, "AMBIGUOUS_PRONUNCIATION_ROLE", 0U);
        }

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
        require_failure("lead.", resources, {}, KGV_INVALID_TEXT,
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
            kgv::PronunciationLexicon unloaded_dictionary;
            kgv::ResolvedFrontendResources invalid = resources;
            invalid.user_dictionary = &unloaded_dictionary;
            require_failure("cat.", invalid, {}, KGV_INVALID_STATE,
                            "FRONTEND_USER_DICTIONARY_NOT_LOADED");
        }
        {
            kgv::ResolvedFrontendResources invalid = resources;
            invalid.user_dictionary = &loaded.lexicon;
            require_failure("cat.", invalid, {}, KGV_ABI_MISMATCH,
                            "FRONTEND_USER_DICTIONARY_ADMISSION_MISMATCH");
        }
        {
            std::vector<kgv::SegmentDefinition> alternate_segments = segments();
            alternate_segments.push_back({"X", 11U});
            const kgv::PronunciationLexicon wrong_inventory =
                load_user_dictionary(alternate_segments);
            kgv::ResolvedFrontendResources invalid = resources;
            invalid.user_dictionary = &wrong_inventory;
            require_failure("cat.", invalid, {}, KGV_ABI_MISMATCH,
                            "FRONTEND_USER_DICTIONARY_INVENTORY_MISMATCH");
        }
        {
            kgv::HeteronymRules unloaded_rules;
            kgv::ResolvedFrontendResources invalid = resources;
            invalid.heteronym_rules = &unloaded_rules;
            require_failure("cat.", invalid, {}, KGV_INVALID_STATE,
                            "FRONTEND_HETERONYM_RULES_NOT_LOADED");
        }
        {
            const kgv::HeteronymRules product_rules = load_heteronyms(
                kgv::PronunciationAdmission::product_admitted,
                loaded.lexicon.resource_sha256(), std::string(64U, 'c'));
            kgv::ResolvedFrontendResources invalid = resources;
            invalid.heteronym_rules = &product_rules;
            require_failure("cat.", invalid, {}, KGV_ABI_MISMATCH,
                            "FRONTEND_HETERONYM_ADMISSION_MISMATCH");
        }
        {
            const std::string other_lexicon(64U, 'b');
            const kgv::HeteronymRules wrong_lexicon = load_heteronyms(
                kgv::PronunciationAdmission::test_fixture, other_lexicon);
            kgv::ResolvedFrontendResources invalid = resources;
            invalid.heteronym_rules = &wrong_lexicon;
            require_failure("cat.", invalid, {}, KGV_ABI_MISMATCH,
                            "FRONTEND_HETERONYM_LEXICON_MISMATCH");
        }
        {
            std::string incompatible_resource = heteronym_fixture(
                kgv::PronunciationAdmission::test_fixture,
                loaded.lexicon.resource_sha256(), {});
            const std::string needle = "\"role\":\"verb\"";
            const std::size_t offset = incompatible_resource.find(needle);
            require(offset != std::string::npos,
                    "heteronym role mismatch fixture drifted");
            incompatible_resource.replace(
                offset, needle.size(), "\"role\":\"adjective\"");
            kgv::HeteronymRules incompatible;
            kgv::HeteronymResourceFailure rule_failure;
            require(kgv::load_heteronym_rules(
                        incompatible_resource,
                        kgv::sha256_hex(incompatible_resource),
                        loaded.lexicon.resource_sha256(),
                        kgv::PronunciationAdmission::test_fixture,
                        &incompatible, &rule_failure) == KGV_OK,
                    "heteronym role mismatch fixture did not load");
            kgv::ResolvedFrontendResources invalid = resources;
            invalid.heteronym_rules = &incompatible;
            require_failure("cat.", invalid, {}, KGV_ABI_MISMATCH,
                            "FRONTEND_HETERONYM_ROLE_MISMATCH");
        }
        {
            kgv::MorphologyRules unloaded_rules;
            kgv::ResolvedFrontendResources invalid = resources;
            invalid.morphology_rules = &unloaded_rules;
            require_failure("cat.", invalid, {}, KGV_INVALID_STATE,
                            "FRONTEND_MORPHOLOGY_RULES_NOT_LOADED");
        }
        {
            const kgv::MorphologyRules product_rules = load_morphology(
                kgv::PronunciationAdmission::product_admitted,
                loaded.lexicon.resource_sha256(), std::string(64U, 'c'));
            kgv::ResolvedFrontendResources invalid = resources;
            invalid.morphology_rules = &product_rules;
            require_failure("cat.", invalid, {}, KGV_ABI_MISMATCH,
                            "FRONTEND_MORPHOLOGY_ADMISSION_MISMATCH");
        }
        {
            const kgv::MorphologyRules wrong_lexicon = load_morphology(
                kgv::PronunciationAdmission::test_fixture,
                std::string(64U, 'b'));
            kgv::ResolvedFrontendResources invalid = resources;
            invalid.morphology_rules = &wrong_lexicon;
            require_failure("cat.", invalid, {}, KGV_ABI_MISMATCH,
                            "FRONTEND_MORPHOLOGY_LEXICON_MISMATCH");
        }
        {
            std::vector<kgv::SegmentDefinition> alternate_segments = segments();
            alternate_segments.push_back({"X", 11U});
            const std::string alternate_inventory_sha =
                kgv::pronunciation_segment_inventory_sha256(
                    alternate_segments);
            const std::string rule_resource = morphology_fixture(
                kgv::PronunciationAdmission::test_fixture,
                loaded.lexicon.resource_sha256(), alternate_inventory_sha,
                {});
            kgv::MorphologyRules wrong_inventory;
            kgv::MorphologyResourceFailure rule_failure;
            require(kgv::load_morphology_rules(
                        rule_resource, kgv::sha256_hex(rule_resource),
                        loaded.lexicon.resource_sha256(),
                        alternate_inventory_sha,
                        kgv::PronunciationAdmission::test_fixture,
                        alternate_segments, &wrong_inventory,
                        &rule_failure) == KGV_OK,
                    "alternate morphology inventory fixture did not load");
            kgv::ResolvedFrontendResources invalid = resources;
            invalid.morphology_rules = &wrong_inventory;
            require_failure("cat.", invalid, {}, KGV_ABI_MISMATCH,
                            "FRONTEND_MORPHOLOGY_INVENTORY_MISMATCH");
        }
        {
            kgv::WeakFormRules unloaded_rules;
            kgv::ResolvedFrontendResources invalid = resources;
            invalid.weak_form_rules = &unloaded_rules;
            require_failure("cat.", invalid, {}, KGV_INVALID_STATE,
                            "FRONTEND_WEAK_FORM_RULES_NOT_LOADED");
        }
        {
            const kgv::WeakFormRules product_rules = load_weak_forms(
                kgv::PronunciationAdmission::product_admitted,
                loaded.lexicon.resource_sha256(), std::string(64U, 'c'));
            kgv::ResolvedFrontendResources invalid = resources;
            invalid.weak_form_rules = &product_rules;
            require_failure("cat.", invalid, {}, KGV_ABI_MISMATCH,
                            "FRONTEND_WEAK_FORM_ADMISSION_MISMATCH");
        }
        {
            const kgv::WeakFormRules wrong_lexicon = load_weak_forms(
                kgv::PronunciationAdmission::test_fixture,
                std::string(64U, 'b'));
            kgv::ResolvedFrontendResources invalid = resources;
            invalid.weak_form_rules = &wrong_lexicon;
            require_failure("cat.", invalid, {}, KGV_ABI_MISMATCH,
                            "FRONTEND_WEAK_FORM_LEXICON_MISMATCH");
        }
        {
            std::vector<kgv::SegmentDefinition> alternate_segments = segments();
            alternate_segments.push_back({"X", 11U});
            const std::string alternate_inventory_sha =
                kgv::pronunciation_segment_inventory_sha256(
                    alternate_segments);
            const std::string rule_resource = weak_form_fixture(
                kgv::PronunciationAdmission::test_fixture,
                loaded.lexicon.resource_sha256(), alternate_inventory_sha,
                {});
            kgv::WeakFormRules wrong_inventory;
            kgv::WeakFormResourceFailure rule_failure;
            require(kgv::load_weak_form_rules(
                        rule_resource, kgv::sha256_hex(rule_resource),
                        loaded.lexicon.resource_sha256(),
                        alternate_inventory_sha,
                        kgv::PronunciationAdmission::test_fixture,
                        alternate_segments, &wrong_inventory,
                        &rule_failure) == KGV_OK,
                    "alternate weak-form inventory fixture did not load");
            kgv::ResolvedFrontendResources invalid = resources;
            invalid.weak_form_rules = &wrong_inventory;
            require_failure("cat.", invalid, {}, KGV_ABI_MISMATCH,
                            "FRONTEND_WEAK_FORM_INVENTORY_MISMATCH");
        }
        {
            const std::string inventory_sha =
                kgv::pronunciation_segment_inventory_sha256(segments());
            std::string incompatible_resource = weak_form_fixture(
                kgv::PronunciationAdmission::test_fixture,
                loaded.lexicon.resource_sha256(), inventory_sha, {});
            const std::string needle = "\"role\":\"weak\"";
            const std::size_t offset = incompatible_resource.find(needle);
            require(offset != std::string::npos,
                    "weak-form role mismatch fixture drifted");
            incompatible_resource.replace(
                offset, needle.size(), "\"role\":\"unreviewed\"");
            kgv::WeakFormRules incompatible;
            kgv::WeakFormResourceFailure rule_failure;
            require(kgv::load_weak_form_rules(
                        incompatible_resource,
                        kgv::sha256_hex(incompatible_resource),
                        loaded.lexicon.resource_sha256(), inventory_sha,
                        kgv::PronunciationAdmission::test_fixture,
                        segments(), &incompatible, &rule_failure) == KGV_OK,
                    "weak-form role mismatch fixture did not load");
            kgv::ResolvedFrontendResources invalid = resources;
            invalid.weak_form_rules = &incompatible;
            require_failure("cat.", invalid, {}, KGV_ABI_MISMATCH,
                            "FRONTEND_WEAK_FORM_ROLE_MISMATCH");
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
        {
            const std::string review_a(64U, 'a');
            const std::string review_b(64U, 'b');
            const LoadedResources product = load_resources(
                kgv::PronunciationAdmission::product_admitted, review_a,
                review_a);
            const kgv::HeteronymRules mismatched_rules = load_heteronyms(
                kgv::PronunciationAdmission::product_admitted,
                product.lexicon.resource_sha256(), review_b);
            require_failure(
                "cat.",
                product.chain(
                    kgv::PronunciationAdmission::product_admitted, nullptr,
                    kFrontendAbi, &mismatched_rules),
                {}, KGV_ABI_MISMATCH,
                "FRONTEND_HETERONYM_REVIEW_MISMATCH");
        }
        {
            const std::string review_a(64U, 'a');
            const std::string review_b(64U, 'b');
            const LoadedResources product = load_resources(
                kgv::PronunciationAdmission::product_admitted, review_a,
                review_a);
            const kgv::MorphologyRules mismatched_rules = load_morphology(
                kgv::PronunciationAdmission::product_admitted,
                product.lexicon.resource_sha256(), review_b);
            require_failure(
                "cat.",
                product.chain(
                    kgv::PronunciationAdmission::product_admitted, nullptr,
                    kFrontendAbi, nullptr, &mismatched_rules),
                {}, KGV_ABI_MISMATCH,
                "FRONTEND_MORPHOLOGY_REVIEW_MISMATCH");
        }
        {
            const std::string review_a(64U, 'a');
            const std::string review_b(64U, 'b');
            const LoadedResources product = load_resources(
                kgv::PronunciationAdmission::product_admitted, review_a,
                review_a);
            const kgv::WeakFormRules mismatched_rules = load_weak_forms(
                kgv::PronunciationAdmission::product_admitted,
                product.lexicon.resource_sha256(), review_b);
            require_failure(
                "cat.",
                product.chain(
                    kgv::PronunciationAdmission::product_admitted, nullptr,
                    kFrontendAbi, nullptr, nullptr, &mismatched_rules),
                {}, KGV_ABI_MISMATCH,
                "FRONTEND_WEAK_FORM_REVIEW_MISMATCH");
        }
        {
            const std::string review(64U, 'a');
            const LoadedResources product = load_resources(
                kgv::PronunciationAdmission::product_admitted, review,
                review);
            const kgv::PronunciationLexicon user_dictionary =
                load_user_dictionary(segments());
            kgv::ResolvedFrontendResult product_user;
            require(kgv::run_resolved_frontend(
                        "cat.", KGV_PROFILE_PROSE,
                        product.chain(
                            kgv::PronunciationAdmission::product_admitted,
                            &user_dictionary),
                        {}, &product_user, &failure) == KGV_OK &&
                        product_user.words.size() == 1U &&
                        product_user.words[0U].pronunciation_source ==
                            kgv::ResolvedPronunciationSource::user_dictionary,
                    "local user dictionary was not admitted above a valid "
                    "product chain");

            const kgv::MorphologyRules product_morphology = load_morphology(
                kgv::PronunciationAdmission::product_admitted,
                product.lexicon.resource_sha256(), review);
            kgv::ResolvedFrontendResult product_inflected;
            require(kgv::run_resolved_frontend(
                        "cats.", KGV_PROFILE_PROSE,
                        product.chain(
                            kgv::PronunciationAdmission::product_admitted,
                            nullptr, kFrontendAbi, nullptr,
                            &product_morphology),
                        {}, &product_inflected, &failure) == KGV_OK &&
                        product_inflected.words.size() == 1U &&
                        product_inflected.words[0U].pronunciation_source ==
                            kgv::ResolvedPronunciationSource::morphology &&
                        product_inflected.words[0U].morphology_stem_source ==
                            kgv::ResolvedPronunciationSource::product_lexicon,
                    "review-bound product morphology was not admitted");

            const kgv::WeakFormRules product_weak_forms = load_weak_forms(
                kgv::PronunciationAdmission::product_admitted,
                product.lexicon.resource_sha256(), review);
            kgv::ResolvedFrontendResult product_weak;
            require(kgv::run_resolved_frontend(
                        "the hour.", KGV_PROFILE_PROSE,
                        product.chain(
                            kgv::PronunciationAdmission::product_admitted,
                            nullptr, kFrontendAbi, nullptr, nullptr,
                            &product_weak_forms),
                        {}, &product_weak, &failure) == KGV_OK &&
                        product_weak.words.size() == 2U &&
                        product_weak.words[0U].pronunciation_source ==
                            kgv::ResolvedPronunciationSource::product_lexicon &&
                        product_weak.words[0U].has_weak_form &&
                        product_weak.words[0U].role == "weak-vowel",
                    "review-bound product weak forms were not admitted");
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
                        first_failure.has_word == second_failure.has_word &&
                        first_failure.override_index ==
                            second_failure.override_index &&
                        first_failure.has_override ==
                            second_failure.has_override,
                    "resolved frontend fuzz result was nondeterministic");
            if (first_status == KGV_OK) {
                require_same(first, second);
            } else {
                require_cleared(first);
                require_cleared(second);
            }
        }

        constexpr std::array<kgv::SourceSpan, 4U> override_spans = {{
            {0U, 3U}, {4U, 7U}, {8U, 11U}, {12U, 15U},
        }};
        for (std::size_t iteration = 0U; iteration < 2000U; ++iteration) {
            std::size_t first_index = static_cast<std::size_t>(
                generator() % override_spans.size());
            std::size_t second_index = static_cast<std::size_t>(
                generator() % override_spans.size());
            if (second_index == first_index) {
                second_index = (second_index + 1U) % override_spans.size();
            }
            if (second_index < first_index) {
                std::swap(first_index, second_index);
            }
            std::vector<kgv::RequestPronunciationOverride> overrides;
            for (std::size_t selected : {first_index, second_index}) {
                kgv::RequestPronunciationOverride entry;
                entry.span = override_spans[selected];
                if ((generator() & 1U) == 0U) {
                    entry.kind = kgv::RequestOverrideKind::replacement_text;
                    entry.replacement_text =
                        (generator() & 1U) == 0U ? "cat" : "bat";
                } else {
                    entry.kind = kgv::RequestOverrideKind::phone_syllables;
                    entry.syllables = {{
                        kgv::SyllableStress::primary,
                        std::vector<std::uint16_t>({
                            static_cast<std::uint16_t>(
                                1U + (generator() % 4U)),
                        }),
                    }};
                }
                overrides.push_back(std::move(entry));
            }
            kgv::ResolvedFrontendResult first;
            kgv::ResolvedFrontendResult second;
            kgv::ResolvedFrontendFailure first_failure;
            kgv::ResolvedFrontendFailure second_failure;
            const int first_status = kgv::run_resolved_frontend(
                "cat bat cat bat.", KGV_PROFILE_PROSE, resources, {},
                overrides, &first, &first_failure);
            const int second_status = kgv::run_resolved_frontend(
                "cat bat cat bat.", KGV_PROFILE_PROSE, resources, {},
                overrides, &second, &second_failure);
            require(first_status == KGV_OK && second_status == KGV_OK &&
                        first_failure.code.empty() &&
                        second_failure.code.empty() &&
                        first.request_override_count == 2U,
                    "valid randomized override request failed");
            require_same(first, second);
        }

        std::cout << "resolved frontend integration tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
