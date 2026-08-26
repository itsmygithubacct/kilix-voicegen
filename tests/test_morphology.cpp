#include "frontend/morphology.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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
        {"S", 1U},  {"SH", 2U}, {"K", 3U}, {"T", 4U}, {"D", 5U},
        {"IH", 6U}, {"Z", 7U},  {"NG", 8U}, {"AE", 9U},
    };
    return value;
}

std::string review_json(std::string_view review) {
    return review.empty() ? "null" : "\"" + std::string(review) + "\"";
}

std::string resource(std::string_view admission = "test-fixture",
                     std::string_view lexicon_sha = kLexiconSha,
                     std::string_view inventory_sha = {},
                     std::string_view review = {}) {
    const std::string inventory = inventory_sha.empty()
                                      ? kgv::pronunciation_segment_inventory_sha256(
                                            segments())
                                      : std::string(inventory_sha);
    return "{\"admission\":\"" + std::string(admission) +
           "\",\"base_lexicon_sha256\":\"" +
           std::string(lexicon_sha) +
           "\",\"dialect\":\"en-AU\","
           "\"past_syllabic_finals\":[\"T\",\"D\"],"
           "\"past_syllabic_suffix\":[\"IH\",\"D\"],"
           "\"past_unvoiced_finals\":[\"S\",\"SH\",\"K\"],"
           "\"past_unvoiced_suffix\":[\"T\"],"
           "\"past_voiced_suffix\":[\"D\"],"
           "\"plural_sibilant_finals\":[\"S\",\"SH\"],"
           "\"plural_sibilant_suffix\":[\"IH\",\"Z\"],"
           "\"plural_unvoiced_finals\":[\"K\",\"T\"],"
           "\"plural_unvoiced_suffix\":[\"S\"],"
           "\"plural_voiced_suffix\":[\"Z\"],"
           "\"progressive_suffix\":[\"IH\",\"NG\"],"
           "\"resource_id\":\"kilix-en-au-morphology-test-1\","
           "\"review_record_sha256\":" + review_json(review) +
           ",\"schema\":\"kilix.voicegen.morphology-rules/v1\","
           "\"segment_inventory_sha256\":\"" + inventory + "\"}\n";
}

int load(std::string_view bytes,
         kgv::PronunciationAdmission admission,
         kgv::MorphologyRules *rules,
         kgv::MorphologyResourceFailure *failure,
         std::string_view lexicon_sha = kLexiconSha,
         const std::vector<kgv::SegmentDefinition> &inventory = segments()) {
    const std::string inventory_sha =
        kgv::pronunciation_segment_inventory_sha256(inventory);
    return kgv::load_morphology_rules(
        bytes, kgv::sha256_hex(bytes), lexicon_sha, inventory_sha, admission,
        inventory, rules, failure);
}

void expect_failure(std::string bytes,
                    kgv::PronunciationAdmission admission,
                    int expected_status,
                    std::string_view expected_code,
                    std::string_view lexicon_sha = kLexiconSha) {
    kgv::MorphologyRules rules;
    kgv::MorphologyResourceFailure failure;
    const int status = load(bytes, admission, &rules, &failure, lexicon_sha);
    require(status == expected_status && failure.status == expected_status,
            "morphology failure returned the wrong status");
    if (failure.code != expected_code) {
        throw std::runtime_error("expected morphology failure " +
                                 std::string(expected_code) + ", got " +
                                 failure.code);
    }
    require(rules.resource_id().empty() &&
                rules.resource_sha256().empty() &&
                rules.base_lexicon_sha256().empty() &&
                rules.segment_inventory_sha256().empty() &&
                rules.review_record_sha256().empty(),
            "failed morphology load retained state");
}

kgv::PronunciationSyllable syllable(
    std::initializer_list<std::uint16_t> ids,
    kgv::SyllableStress stress = kgv::SyllableStress::primary) {
    return kgv::PronunciationSyllable{stress,
                                      std::vector<std::uint16_t>(ids)};
}

void require_apply(const kgv::MorphologyRules &rules,
                   kgv::MorphologyKind kind,
                   std::initializer_list<std::uint16_t> stem_ids,
                   const std::vector<kgv::PronunciationSyllable> &expected) {
    std::vector<kgv::PronunciationSyllable> result;
    kgv::MorphologyApplyFailure failure;
    require(rules.apply(kind, {syllable(stem_ids)}, &result, &failure) ==
                KGV_OK &&
                failure.code.empty() && result.size() == expected.size(),
            "valid morphology application failed");
    for (std::size_t index = 0U; index < result.size(); ++index) {
        require(result[index].stress == expected[index].stress &&
                    result[index].segment_ids == expected[index].segment_ids,
                "morphology allomorph output changed");
    }
}

bool has_candidate(const std::vector<kgv::MorphologyCandidate> &candidates,
                   kgv::MorphologyKind kind,
                   std::string_view stem) {
    for (const kgv::MorphologyCandidate &candidate : candidates) {
        if (candidate.kind == kind && candidate.stem == stem) {
            return true;
        }
    }
    return false;
}

void require_candidate(std::string_view word,
                       kgv::MorphologyKind kind,
                       std::string_view stem) {
    require(has_candidate(kgv::morphology_candidates(word), kind, stem),
            "expected orthographic stem candidate is absent");
}

void replace_once(std::string *value,
                  std::string_view before,
                  std::string_view after) {
    const std::size_t offset = value->find(before);
    require(offset != std::string::npos,
            "morphology mutation fixture drifted");
    value->replace(offset, before.size(), after);
}

void require_same_candidates(
    const std::vector<kgv::MorphologyCandidate> &left,
    const std::vector<kgv::MorphologyCandidate> &right) {
    require(left.size() == right.size(),
            "morphology candidate count changed across replay");
    for (std::size_t index = 0U; index < left.size(); ++index) {
        require(left[index].kind == right[index].kind &&
                    left[index].stem == right[index].stem,
                "morphology candidate changed across replay");
    }
}

}  // namespace

int main() {
    try {
        const std::string bytes = resource();
        const std::string inventory_sha =
            kgv::pronunciation_segment_inventory_sha256(segments());
        kgv::MorphologyRules rules;
        kgv::MorphologyResourceFailure failure;
        require(load(bytes, kgv::PronunciationAdmission::test_fixture,
                     &rules, &failure) == KGV_OK,
                "valid morphology fixture was rejected");
        require(failure.code.empty() &&
                    rules.resource_id() ==
                        "kilix-en-au-morphology-test-1" &&
                    rules.resource_sha256() == kgv::sha256_hex(bytes) &&
                    rules.base_lexicon_sha256() == kLexiconSha &&
                    rules.segment_inventory_sha256() == inventory_sha &&
                    rules.review_record_sha256().empty() &&
                    rules.admission() ==
                        kgv::PronunciationAdmission::test_fixture,
                "morphology resource metadata changed");

        require_apply(rules, kgv::MorphologyKind::plural_or_possessive,
                      {1U}, {syllable({1U}),
                             syllable({6U, 7U},
                                      kgv::SyllableStress::none)});
        require_apply(rules, kgv::MorphologyKind::plural_or_possessive,
                      {3U}, {syllable({3U, 1U})});
        require_apply(rules, kgv::MorphologyKind::plural_or_possessive,
                      {5U}, {syllable({5U, 7U})});
        require_apply(rules, kgv::MorphologyKind::past, {4U},
                      {syllable({4U}),
                       syllable({6U, 5U}, kgv::SyllableStress::none)});
        require_apply(rules, kgv::MorphologyKind::past, {3U},
                      {syllable({3U, 4U})});
        require_apply(rules, kgv::MorphologyKind::past, {9U},
                      {syllable({9U, 5U})});
        require_apply(rules, kgv::MorphologyKind::progressive, {9U},
                      {syllable({9U}),
                       syllable({6U, 8U}, kgv::SyllableStress::none)});

        require_candidate("cats", kgv::MorphologyKind::plural_or_possessive,
                          "cat");
        require_candidate("boxes", kgv::MorphologyKind::plural_or_possessive,
                          "box");
        require_candidate("cities", kgv::MorphologyKind::plural_or_possessive,
                          "city");
        require_candidate("cat's", kgv::MorphologyKind::plural_or_possessive,
                          "cat");
        require_candidate("cat\xe2\x80\x99s",
                          kgv::MorphologyKind::plural_or_possessive, "cat");
        require_candidate("loved", kgv::MorphologyKind::past, "love");
        require_candidate("stopped", kgv::MorphologyKind::past, "stop");
        require_candidate("tried", kgv::MorphologyKind::past, "try");
        require_candidate("panicked", kgv::MorphologyKind::past, "panic");
        require_candidate("walking", kgv::MorphologyKind::progressive,
                          "walk");
        require_candidate("making", kgv::MorphologyKind::progressive,
                          "make");
        require_candidate("running", kgv::MorphologyKind::progressive,
                          "run");
        require_candidate("panicking", kgv::MorphologyKind::progressive,
                          "panic");
        require(kgv::morphology_candidates("class").empty() &&
                    kgv::morphology_candidates(std::string(257U, 'a')).empty(),
                "bounded morphology admitted an unsupported surface form");
        require(std::string(kgv::morphology_kind_name(
                    kgv::MorphologyKind::plural_or_possessive)) ==
                    "PLURAL_OR_POSSESSIVE" &&
                    std::string(kgv::morphology_kind_name(
                        kgv::MorphologyKind::past)) == "PAST" &&
                    std::string(kgv::morphology_kind_name(
                        kgv::MorphologyKind::progressive)) == "PROGRESSIVE" &&
                    std::string(kgv::morphology_kind_name(
                        static_cast<kgv::MorphologyKind>(99))) == "UNKNOWN",
                "morphology kind names changed");

        {
            kgv::MorphologyRules rejected = rules;
            kgv::MorphologyResourceFailure hash_failure;
            const int status = kgv::load_morphology_rules(
                bytes, std::string(64U, '0'), kLexiconSha, inventory_sha,
                kgv::PronunciationAdmission::test_fixture, segments(),
                &rejected, &hash_failure);
            require(status == KGV_HASH_MISMATCH &&
                        hash_failure.code ==
                            "MORPHOLOGY_RESOURCE_HASH_MISMATCH" &&
                        rejected.resource_sha256().empty(),
                    "morphology hash mismatch retained loaded state");
        }
        expect_failure(bytes,
                       kgv::PronunciationAdmission::product_admitted,
                       KGV_INVALID_MODEL,
                       "MORPHOLOGY_RESOURCE_ADMISSION");
        expect_failure(bytes, kgv::PronunciationAdmission::test_fixture,
                       KGV_ABI_MISMATCH,
                       "MORPHOLOGY_RESOURCE_LEXICON_MISMATCH",
                       std::string(64U, 'b'));
        expect_failure(resource("product-admitted"),
                       kgv::PronunciationAdmission::product_admitted,
                       KGV_INVALID_MODEL,
                       "MORPHOLOGY_RESOURCE_ADMISSION");
        {
            const std::string review(64U, 'c');
            const std::string product =
                resource("product-admitted", kLexiconSha, {}, review);
            kgv::MorphologyRules product_rules;
            require(load(product,
                         kgv::PronunciationAdmission::product_admitted,
                         &product_rules, &failure) == KGV_OK &&
                        product_rules.review_record_sha256() == review,
                    "review-bound product morphology was rejected");
        }
        expect_failure(resource("test-fixture", kLexiconSha, {},
                                std::string(64U, 'c')),
                       kgv::PronunciationAdmission::test_fixture,
                       KGV_INVALID_MODEL,
                       "MORPHOLOGY_RESOURCE_ADMISSION");

        {
            std::string changed = bytes;
            replace_once(&changed, "[\"K\",\"T\"]",
                         "[\"S\",\"K\"]");
            expect_failure(changed,
                           kgv::PronunciationAdmission::test_fixture,
                           KGV_INVALID_MODEL,
                           "MORPHOLOGY_RESOURCE_CLASS_OVERLAP");
        }
        {
            std::string changed = bytes;
            replace_once(&changed, "[\"K\",\"T\"]",
                         "[\"T\",\"K\"]");
            expect_failure(changed,
                           kgv::PronunciationAdmission::test_fixture,
                           KGV_INVALID_MODEL,
                           "MORPHOLOGY_RESOURCE_ORDER");
        }
        {
            std::string changed = bytes;
            replace_once(&changed, "[\"IH\",\"NG\"]",
                         "[\"IH\",\"NOPE\"]");
            expect_failure(changed,
                           kgv::PronunciationAdmission::test_fixture,
                           KGV_INVALID_MODEL, "MORPHOLOGY_UNKNOWN_SEGMENT");
        }
        expect_failure(resource("test-fixture", kLexiconSha,
                                std::string(64U, 'b')),
                       kgv::PronunciationAdmission::test_fixture,
                       KGV_ABI_MISMATCH,
                       "MORPHOLOGY_RESOURCE_INVENTORY_MISMATCH");
        expect_failure(bytes.substr(0U, bytes.size() - 1U),
                       kgv::PronunciationAdmission::test_fixture,
                       KGV_INVALID_MODEL,
                       "MORPHOLOGY_RESOURCE_CANONICAL_FORM");
        expect_failure(bytes.substr(0U, bytes.size() - 1U) + "\r\n",
                       kgv::PronunciationAdmission::test_fixture,
                       KGV_INVALID_MODEL,
                       "MORPHOLOGY_RESOURCE_CANONICAL_FORM");
        expect_failure(std::string(65537U, 'x'),
                       kgv::PronunciationAdmission::test_fixture,
                       KGV_RESOURCE_EXHAUSTED,
                       "MORPHOLOGY_RESOURCE_TOO_LARGE");

        {
            std::vector<kgv::PronunciationSyllable> output = {
                syllable({9U})};
            kgv::MorphologyApplyFailure apply_failure;
            kgv::MorphologyRules unloaded;
            require(unloaded.apply(kgv::MorphologyKind::past,
                                   {syllable({9U})}, &output,
                                   &apply_failure) == KGV_INVALID_STATE &&
                        apply_failure.code ==
                            "MORPHOLOGY_RULES_NOT_LOADED" &&
                        output.empty(),
                    "unloaded morphology did not fail without output");
            output = {syllable({9U})};
            require(rules.apply(kgv::MorphologyKind::past, {}, &output,
                                &apply_failure) == KGV_INVALID_ARGUMENT &&
                        apply_failure.code == "INVALID_MORPHOLOGY_STEM" &&
                        output.empty(),
                    "invalid morphology stem retained output");
            output = {syllable({9U})};
            require(rules.apply(kgv::MorphologyKind::past,
                                {syllable({99U})}, &output,
                                &apply_failure) == KGV_INVALID_ARGUMENT &&
                        apply_failure.code == "INVALID_MORPHOLOGY_STEM" &&
                        output.empty(),
                    "unknown stem segment retained morphology output");
            output = {syllable({9U})};
            require(rules.apply(static_cast<kgv::MorphologyKind>(99),
                                {syllable({9U})}, &output,
                                &apply_failure) == KGV_INVALID_ARGUMENT &&
                        apply_failure.code == "UNKNOWN_MORPHOLOGY_KIND" &&
                        output.empty(),
                    "unknown morphology kind retained output");

            std::vector<kgv::PronunciationSyllable> full_word(
                16U, syllable({9U}, kgv::SyllableStress::none));
            full_word.front().stress = kgv::SyllableStress::primary;
            output = {syllable({9U})};
            require(rules.apply(
                        kgv::MorphologyKind::progressive, full_word,
                        &output, &apply_failure) == KGV_INVALID_TEXT &&
                        apply_failure.code ==
                            "MORPHOLOGY_RESULT_TOO_LARGE" &&
                        output.empty(),
                    "oversized morphology result retained output");
        }

        const std::vector<std::string> surfaces = {
            "cats",     "boxes",   "cities",   "loved",   "stopped",
            "tried",    "panicked", "walking", "making",  "running",
            "panicking", "class",   "cat's",    "unknown",
        };
        const std::vector<std::uint16_t> finals = {
            1U, 2U, 3U, 4U, 5U, 7U, 9U,
        };
        std::mt19937 generator(0x4d4f5250U);
        for (std::size_t iteration = 0U; iteration < 2000U; ++iteration) {
            const std::string &surface =
                surfaces[generator() % surfaces.size()];
            require_same_candidates(kgv::morphology_candidates(surface),
                                    kgv::morphology_candidates(surface));
            const auto kind = static_cast<kgv::MorphologyKind>(
                generator() % 3U);
            const std::uint16_t final =
                finals[generator() % finals.size()];
            std::vector<kgv::PronunciationSyllable> first;
            std::vector<kgv::PronunciationSyllable> second;
            kgv::MorphologyApplyFailure first_failure;
            kgv::MorphologyApplyFailure second_failure;
            require(rules.apply(kind, {syllable({9U, final})}, &first,
                                &first_failure) == KGV_OK &&
                        rules.apply(kind, {syllable({9U, final})}, &second,
                                    &second_failure) == KGV_OK &&
                        first.size() == second.size(),
                    "bounded morphology replay failed");
            for (std::size_t index = 0U; index < first.size(); ++index) {
                require(first[index].stress == second[index].stress &&
                            first[index].segment_ids ==
                                second[index].segment_ids,
                        "morphology application changed across replay");
            }
        }

        std::cout << "morphology resource tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
