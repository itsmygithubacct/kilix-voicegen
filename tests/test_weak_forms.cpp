#include "frontend/weak_forms.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "frontend/pronunciation.h"
#include "kilix_voicegen.h"
#include "runtime/sha256.h"

namespace {

constexpr std::string_view kLexiconSha =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

const std::vector<kgv::SegmentDefinition> &segments() {
    static const std::vector<kgv::SegmentDefinition> value = {
        {"K", 1U}, {"AE", 2U}, {"T", 3U}, {"IH", 4U}, {"D", 5U},
    };
    return value;
}

std::string review_json(std::string_view review) {
    return review.empty() ? "null" : "\"" + std::string(review) + "\"";
}

std::string rule(std::string_view id,
                 std::string_view target,
                 std::string_view role,
                 std::string_view position,
                 std::string_view next_segment,
                 std::string_view capitalization = "any") {
    return "{\"capitalization\":\"" + std::string(capitalization) +
           "\",\"next_segment\":\"" + std::string(next_segment) +
           "\",\"position\":\"" + std::string(position) +
           "\",\"role\":\"" + std::string(role) +
           "\",\"rule_id\":\"" + std::string(id) +
           "\",\"schema\":\"kilix.voicegen.weak-form-rule/v1\"," +
           "\"source\":\"project-test-fixture\",\"target\":\"" +
           std::string(target) + "\"}\n";
}

std::string resource(
    std::string_view admission = "test-fixture",
    std::string_view lexicon_sha = kLexiconSha,
    std::string_view inventory_sha = {},
    std::string_view review = {},
    std::string_view vowels = "[\"AE\",\"IH\"]",
    bool ambiguous = false) {
    const std::string actual_inventory = inventory_sha.empty()
                                             ? kgv::pronunciation_segment_inventory_sha256(
                                                   segments())
                                             : std::string(inventory_sha);
    const std::size_t entry_count = ambiguous ? 5U : 4U;
    std::string bytes =
        "{\"admission\":\"" + std::string(admission) +
        "\",\"base_lexicon_sha256\":\"" + std::string(lexicon_sha) +
        "\",\"dialect\":\"en-AU\",\"entry_count\":" +
        std::to_string(entry_count) +
        ",\"resource_id\":\"kilix-en-au-weak-form-test-1\"," +
        "\"review_record_sha256\":" + review_json(review) +
        ",\"schema\":\"kilix.voicegen.weak-form-rules/v1\"," +
        "\"segment_inventory_sha256\":\"" + actual_inventory +
        "\",\"vowel_segments\":" + std::string(vowels) + "}\n";
    bytes += rule("fixture.a.medial", "a", "weak", "phrase-medial",
                  "any");
    bytes += rule("fixture.the.before-consonant", "the", "weak-consonant",
                  "phrase-medial", "non-vowel");
    bytes += rule("fixture.the.before-vowel", "the", "weak-vowel",
                  "phrase-medial", "vowel");
    if (ambiguous) {
        bytes += rule("fixture.the.medial", "the", "weak-consonant",
                      "phrase-medial", "any");
    }
    bytes += rule("fixture.to.medial", "to", "weak", "phrase-medial",
                  "any");
    return bytes;
}

std::string pronunciation_entry(std::string_view grapheme,
                                std::string_view role,
                                std::string_view segment) {
    return "{\"case\":\"ascii-fold\",\"grapheme\":\"" +
           std::string(grapheme) + "\",\"roles\":[\"" +
           std::string(role) +
           "\"],\"schema\":\"kilix.voicegen.pronunciation-entry/v1\"," +
           "\"source\":\"project-test-fixture\",\"syllables\":[{" +
           "\"segments\":[\"" + std::string(segment) +
           "\"],\"stress\":\"none\"}]}\n";
}

kgv::PronunciationLexicon lexicon(bool complete) {
    const std::string inventory_sha =
        kgv::pronunciation_segment_inventory_sha256(segments());
    const std::size_t entry_count = complete ? 7U : 6U;
    std::string bytes =
        std::string("{\"admission\":\"test-fixture\",\"dialect\":\"en-AU\",") +
        "\"entry_count\":" + std::to_string(entry_count) +
        ",\"resource_id\":\"kilix-en-au-weak-form-lexicon-test-1\"," +
        "\"review_record_sha256\":null," +
        "\"schema\":\"kilix.voicegen.pronunciation-lexicon/v1\"," +
        "\"segment_inventory_sha256\":\"" + inventory_sha + "\"}\n";
    bytes += pronunciation_entry("a", "default", "AE");
    bytes += pronunciation_entry("a", "weak", "IH");
    bytes += pronunciation_entry("the", "default", "AE");
    bytes += pronunciation_entry("the", "weak-consonant", "D");
    bytes += pronunciation_entry("the", "weak-vowel", "IH");
    bytes += pronunciation_entry("to", "default", "T");
    if (complete) {
        bytes += pronunciation_entry("to", "weak", "IH");
    }
    kgv::PronunciationLexicon loaded;
    kgv::PronunciationResourceFailure failure;
    require(kgv::load_pronunciation_lexicon(
                bytes, kgv::sha256_hex(bytes), inventory_sha,
                kgv::PronunciationAdmission::test_fixture, segments(),
                &loaded, &failure) == KGV_OK,
            "weak-form compatibility lexicon did not load");
    return loaded;
}

int load(std::string_view bytes,
         kgv::PronunciationAdmission admission,
         kgv::WeakFormRules *rules,
         kgv::WeakFormResourceFailure *failure,
         std::string_view lexicon_sha = kLexiconSha,
         std::string_view inventory_sha = {}) {
    const std::string actual_inventory = inventory_sha.empty()
                                             ? kgv::pronunciation_segment_inventory_sha256(
                                                   segments())
                                             : std::string(inventory_sha);
    return kgv::load_weak_form_rules(
        bytes, kgv::sha256_hex(bytes), lexicon_sha, actual_inventory,
        admission, segments(), rules, failure);
}

void expect_failure(std::string bytes,
                    kgv::PronunciationAdmission admission,
                    int status,
                    std::string_view code,
                    std::string_view lexicon_sha = kLexiconSha,
                    std::string_view inventory_sha = {}) {
    kgv::WeakFormRules rules;
    kgv::WeakFormResourceFailure failure;
    const int actual = load(bytes, admission, &rules, &failure, lexicon_sha,
                            inventory_sha);
    require(actual == status && failure.status == status &&
                failure.code == code && rules.resource_sha256().empty(),
            std::string("unexpected weak-form failure for ") +
                std::string(code));
}

void replace_once(std::string *value,
                  std::string_view needle,
                  std::string_view replacement) {
    const std::size_t offset = value->find(needle);
    require(offset != std::string::npos,
            "weak-form mutation needle was not found");
    value->replace(offset, needle.size(), replacement);
}

bool same_decision(const kgv::WeakFormDecision &left,
                   const kgv::WeakFormDecision &right) {
    return left.kind == right.kind && left.role == right.role &&
           left.rule_id == right.rule_id &&
           left.match_count == right.match_count;
}

}  // namespace

int main() {
    try {
        const std::string inventory_sha =
            kgv::pronunciation_segment_inventory_sha256(segments());
        require(inventory_sha.size() == 64U,
                "weak-form inventory hash was not produced");
        const std::string bytes = resource();
        kgv::WeakFormRules rules;
        kgv::WeakFormResourceFailure failure;
        require(load(bytes, kgv::PronunciationAdmission::test_fixture, &rules,
                     &failure) == KGV_OK,
                "valid weak-form resource was rejected");
        require(rules.resource_id() == "kilix-en-au-weak-form-test-1" &&
                    rules.resource_sha256() == kgv::sha256_hex(bytes) &&
                    rules.base_lexicon_sha256() == kLexiconSha &&
                    rules.segment_inventory_sha256() == inventory_sha &&
                    rules.review_record_sha256().empty() &&
                    rules.admission() ==
                        kgv::PronunciationAdmission::test_fixture &&
                    rules.rule_count() == 4U,
                "weak-form metadata changed");
        require(rules.contains_target("THE") &&
                    !rules.contains_target("other"),
                "weak-form target index changed");
        require(rules.compatible_with(lexicon(true)) &&
                    !rules.compatible_with(lexicon(false)),
                "weak-form lexicon compatibility changed");

        const kgv::WeakFormDecision article =
            rules.decide("a", 0U, 0U, 2U, true, 1U);
        require(article.kind == kgv::WeakFormDecisionKind::matched &&
                    article.role == "weak" &&
                    article.rule_id == "fixture.a.medial" &&
                    article.match_count == 1U,
                "generic phrase-medial weak form changed");
        const kgv::WeakFormDecision before_vowel =
            rules.decide("the", 0U, 0U, 2U, true, 2U);
        require(before_vowel.kind == kgv::WeakFormDecisionKind::matched &&
                    before_vowel.role == "weak-vowel" &&
                    before_vowel.rule_id == "fixture.the.before-vowel",
                "pre-vocalic weak form changed");
        const kgv::WeakFormDecision before_consonant =
            rules.decide("The", 0U, 0U, 2U, true, 1U);
        require(before_consonant.kind ==
                        kgv::WeakFormDecisionKind::matched &&
                    before_consonant.role == "weak-consonant",
                "pre-consonantal weak form changed");
        require(rules.decide("to", 0U, 0U, 1U, false, 0U).kind ==
                    kgv::WeakFormDecisionKind::no_match,
                "phrase-final strong form no longer wins");
        require(rules.decide("other", 0U, 0U, 1U, false, 0U).kind ==
                    kgv::WeakFormDecisionKind::not_target,
                "non-target weak-form decision changed");
        require(rules.decide("the", 1U, 0U, 1U, false, 0U).kind ==
                    kgv::WeakFormDecisionKind::not_target &&
                    rules.decide("the", 0U, 0U, 2U, false, 0U).kind ==
                        kgv::WeakFormDecisionKind::not_target &&
                    rules.decide("the", 0U, 0U, 2U, true, 99U).kind ==
                        kgv::WeakFormDecisionKind::not_target,
                "invalid weak-form context was not rejected safely");
        require(std::string(kgv::weak_form_decision_name(
                    kgv::WeakFormDecisionKind::matched)) == "MATCHED",
                "weak-form decision name changed");

        {
            const std::string ambiguous_bytes = resource(
                "test-fixture", kLexiconSha, inventory_sha, {},
                "[\"AE\",\"IH\"]", true);
            kgv::WeakFormRules ambiguous;
            require(load(ambiguous_bytes,
                         kgv::PronunciationAdmission::test_fixture,
                         &ambiguous, &failure) == KGV_OK,
                    "overlap fixture did not load");
            const kgv::WeakFormDecision decision =
                ambiguous.decide("the", 0U, 0U, 2U, true, 2U);
            require(decision.kind == kgv::WeakFormDecisionKind::ambiguous &&
                        decision.role.empty() && decision.rule_id.empty() &&
                        decision.match_count == 2U,
                    "weak-form overlap did not fail closed");
        }

        {
            kgv::WeakFormRules rejected = rules;
            kgv::WeakFormResourceFailure hash_failure;
            const int status = kgv::load_weak_form_rules(
                bytes, std::string(64U, '0'), kLexiconSha, inventory_sha,
                kgv::PronunciationAdmission::test_fixture, segments(),
                &rejected, &hash_failure);
            require(status == KGV_HASH_MISMATCH &&
                        hash_failure.code ==
                            "WEAK_FORM_RESOURCE_HASH_MISMATCH" &&
                        rejected.resource_sha256().empty(),
                    "weak-form hash mismatch retained loaded state");
        }
        expect_failure(bytes, kgv::PronunciationAdmission::product_admitted,
                       KGV_INVALID_MODEL, "WEAK_FORM_RESOURCE_ADMISSION");
        expect_failure(bytes, kgv::PronunciationAdmission::test_fixture,
                       KGV_ABI_MISMATCH,
                       "WEAK_FORM_RESOURCE_LEXICON_MISMATCH",
                       std::string(64U, 'b'));
        expect_failure(resource("test-fixture", kLexiconSha,
                                std::string(64U, 'b')),
                       kgv::PronunciationAdmission::test_fixture,
                       KGV_ABI_MISMATCH,
                       "WEAK_FORM_RESOURCE_INVENTORY_MISMATCH");
        expect_failure(resource("product-admitted"),
                       kgv::PronunciationAdmission::product_admitted,
                       KGV_INVALID_MODEL, "WEAK_FORM_RESOURCE_ADMISSION");
        {
            const std::string review(64U, 'c');
            const std::string product = resource(
                "product-admitted", kLexiconSha, inventory_sha, review);
            kgv::WeakFormRules product_rules;
            require(load(product,
                         kgv::PronunciationAdmission::product_admitted,
                         &product_rules, &failure) == KGV_OK &&
                        product_rules.review_record_sha256() == review,
                    "review-bound product weak forms were rejected");
        }
        expect_failure(resource("test-fixture", kLexiconSha, inventory_sha,
                                std::string(64U, 'c')),
                       kgv::PronunciationAdmission::test_fixture,
                       KGV_INVALID_MODEL, "WEAK_FORM_RESOURCE_ADMISSION");
        expect_failure(resource("test-fixture", kLexiconSha, inventory_sha,
                                {}, "[\"AE\",\"NOPE\"]"),
                       kgv::PronunciationAdmission::test_fixture,
                       KGV_INVALID_MODEL, "WEAK_FORM_UNKNOWN_SEGMENT");
        expect_failure(resource("test-fixture", kLexiconSha, inventory_sha,
                                {}, "[\"IH\",\"AE\"]"),
                       kgv::PronunciationAdmission::test_fixture,
                       KGV_INVALID_MODEL, "WEAK_FORM_RESOURCE_ORDER");
        expect_failure(resource("test-fixture", kLexiconSha, inventory_sha,
                                {}, "[\"K\",\"AE\",\"T\",\"IH\",\"D\"]"),
                       kgv::PronunciationAdmission::test_fixture,
                       KGV_INVALID_MODEL, "WEAK_FORM_RESOURCE_LIMIT");
        {
            std::string changed = bytes;
            replace_once(
                &changed,
                "\"position\":\"phrase-medial\",\"role\":\"weak-consonant\"",
                "\"position\":\"phrase-final\",\"role\":\"weak-consonant\"");
            expect_failure(changed,
                           kgv::PronunciationAdmission::test_fixture,
                           KGV_INVALID_MODEL,
                           "WEAK_FORM_RESOURCE_CONDITION");
        }
        {
            std::string changed = bytes;
            replace_once(&changed, "\"target\":\"a\"",
                         "\"target\":\"z\"");
            expect_failure(changed,
                           kgv::PronunciationAdmission::test_fixture,
                           KGV_INVALID_MODEL, "WEAK_FORM_RESOURCE_ORDER");
        }
        {
            std::string changed = bytes;
            replace_once(&changed, "\"entry_count\":4",
                         "\"entry_count\":3");
            expect_failure(changed,
                           kgv::PronunciationAdmission::test_fixture,
                           KGV_INVALID_MODEL,
                           "WEAK_FORM_RESOURCE_ENTRY_COUNT");
        }
        expect_failure(bytes.substr(0U, bytes.size() - 1U),
                       kgv::PronunciationAdmission::test_fixture,
                       KGV_INVALID_MODEL,
                       "WEAK_FORM_RESOURCE_CANONICAL_FORM");
        expect_failure(bytes.substr(0U, bytes.size() - 1U) + "\r\n",
                       kgv::PronunciationAdmission::test_fixture,
                       KGV_INVALID_MODEL,
                       "WEAK_FORM_RESOURCE_CANONICAL_FORM");
        expect_failure(std::string((4U * 1024U * 1024U) + 1U, 'x'),
                       kgv::PronunciationAdmission::test_fixture,
                       KGV_RESOURCE_EXHAUSTED,
                       "WEAK_FORM_RESOURCE_TOO_LARGE");

        std::mt19937 generator(0x5745414bU);
        const std::vector<std::string> words = {"a", "the", "to", "other"};
        const std::vector<std::uint16_t> next_segments = {1U, 2U, 3U, 4U, 5U};
        for (std::size_t iteration = 0U; iteration < 2000U; ++iteration) {
            const std::string &word = words[generator() % words.size()];
            const std::uint16_t next =
                next_segments[generator() % next_segments.size()];
            const bool medial = (generator() & 1U) != 0U;
            const kgv::WeakFormDecision first = rules.decide(
                word, 0U, 0U, medial ? 2U : 1U, medial, next);
            const kgv::WeakFormDecision second = rules.decide(
                word, 0U, 0U, medial ? 2U : 1U, medial, next);
            require(same_decision(first, second),
                    "weak-form decision changed across deterministic replay");
        }

        std::cout << "weak-form resource tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
