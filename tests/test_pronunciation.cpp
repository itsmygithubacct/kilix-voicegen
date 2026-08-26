#include "frontend/pronunciation.h"

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

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

const std::vector<kgv::SegmentDefinition> &segments() {
    static const std::vector<kgv::SegmentDefinition> value = {
        {"AO", 1U}, {"D", 2U}, {"EH", 3U}, {"HH", 4U}, {"IH", 5U},
        {"K", 6U}, {"L", 7U}, {"OW", 8U}, {"R", 9U}, {"S", 10U},
    };
    return value;
}

std::string header(std::string_view admission,
                   std::size_t count,
                   std::string_view inventory_sha256,
                   std::string_view review = {}) {
    const std::string review_json = review.empty()
        ? "null"
        : "\"" + std::string(review) + "\"";
    return "{\"admission\":\"" + std::string(admission) +
           "\",\"dialect\":\"en-AU\",\"entry_count\":" +
           std::to_string(count) +
           ",\"resource_id\":\"kilix-en-au-test-1\"," +
           "\"review_record_sha256\":" + review_json +
           ",\"schema\":\"kilix.voicegen.pronunciation-lexicon/v1\"," +
           "\"segment_inventory_sha256\":\"" +
           std::string(inventory_sha256) + "\"}\n";
}

std::string entry(std::string_view grapheme,
                  std::string_view case_mode,
                  std::string_view roles_json,
                  std::string_view syllables_json,
                  std::string_view suffix = {}) {
    return "{\"case\":\"" + std::string(case_mode) +
           "\",\"grapheme\":\"" + std::string(grapheme) +
           "\",\"roles\":" + std::string(roles_json) +
           ",\"schema\":\"kilix.voicegen.pronunciation-entry/v1\"," +
           "\"source\":\"project-test-fixture\",\"syllables\":" +
           std::string(syllables_json) + std::string(suffix) + "}\n";
}

std::string valid_fixture(std::string_view inventory_sha256) {
    std::string resource = header("test-fixture", 4U, inventory_sha256);
    resource += entry(
        "Kilix", "exact", "[\"default\",\"proper\"]",
        "[{\"segments\":[\"K\",\"IH\"],\"stress\":\"primary\"},"
        "{\"segments\":[\"L\",\"IH\",\"K\",\"S\"],\"stress\":\"none\"}]");
    resource += entry(
        "hello", "ascii-fold", "[\"default\"]",
        "[{\"segments\":[\"HH\",\"EH\"],\"stress\":\"none\"},"
        "{\"segments\":[\"L\",\"OW\"],\"stress\":\"primary\"}]");
    resource += entry(
        "record", "ascii-fold", "[\"noun\"]",
        "[{\"segments\":[\"R\",\"EH\"],\"stress\":\"primary\"},"
        "{\"segments\":[\"K\",\"AO\",\"D\"],\"stress\":\"none\"}]");
    resource += entry(
        "record", "ascii-fold", "[\"verb\"]",
        "[{\"segments\":[\"R\",\"IH\"],\"stress\":\"none\"},"
        "{\"segments\":[\"K\",\"AO\",\"D\"],\"stress\":\"primary\"}]");
    return resource;
}

int load(std::string_view resource,
         kgv::PronunciationAdmission admission,
         kgv::PronunciationLexicon *lexicon,
         kgv::PronunciationResourceFailure *failure,
         std::string_view inventory_sha256) {
    return kgv::load_pronunciation_lexicon(
        resource, kgv::sha256_hex(resource), inventory_sha256, admission,
        segments(), lexicon, failure);
}

void expect_failure(std::string resource,
                    kgv::PronunciationAdmission admission,
                    std::string_view inventory_sha256,
                    int expected_status,
                    std::string_view expected_code) {
    kgv::PronunciationLexicon lexicon;
    kgv::PronunciationResourceFailure failure;
    const int status = load(resource, admission, &lexicon, &failure,
                            inventory_sha256);
    require(status == expected_status, "failure returned the wrong status");
    require(failure.status == status, "failure record status differs");
    if (failure.code != expected_code) {
        throw std::runtime_error("expected failure code " +
                                 std::string(expected_code) + ", got " +
                                 failure.code);
    }
    require(lexicon.entry_count() == 0U, "failed load retained entries");
    require(lexicon.resource_id().empty(), "failed load retained metadata");
}

}  // namespace

int main() {
    try {
        const std::string inventory_sha256 =
            kgv::pronunciation_segment_inventory_sha256(segments());
        require(inventory_sha256.size() == 64U,
                "canonical inventory hash was not produced");
        require(kgv::pronunciation_segment_inventory_sha256(
                    {{"AO", 2U}, {"D", 1U}}).empty(),
                "out-of-order segment IDs produced an inventory hash");
        require(kgv::pronunciation_segment_inventory_sha256(
                    {{"AO", 1U}, {"AO", 2U}}).empty(),
                "duplicate segment names produced an inventory hash");

        const std::string resource = valid_fixture(inventory_sha256);
        kgv::PronunciationLexicon lexicon;
        kgv::PronunciationResourceFailure failure;
        require(load(resource, kgv::PronunciationAdmission::test_fixture,
                     &lexicon, &failure, inventory_sha256) == KGV_OK,
                "valid pronunciation fixture was rejected");
        require(failure.code.empty(), "valid load retained a failure");
        require(lexicon.entry_count() == 4U, "valid entry count changed");
        require(lexicon.resource_id() == "kilix-en-au-test-1",
                "resource ID changed");
        require(lexicon.resource_sha256() == kgv::sha256_hex(resource),
                "resource hash was not retained");
        require(lexicon.segment_inventory_sha256() == inventory_sha256,
                "inventory hash was not retained");
        require(lexicon.review_record_sha256().empty(),
                "test fixture acquired a review record");
        require(lexicon.admission() == kgv::PronunciationAdmission::test_fixture,
                "admission changed");

        const kgv::PronunciationEntry *kilix = lexicon.find("Kilix", "proper");
        require(kilix != nullptr, "exact role lookup failed");
        require(kilix->syllables.size() == 2U, "syllables were not retained");
        require(kilix->syllables[0U].stress == kgv::SyllableStress::primary,
                "stress was not retained");
        require(kilix->syllables[0U].segment_ids ==
                    std::vector<std::uint16_t>({6U, 5U}),
                "segments were not converted to IDs");
        require(lexicon.find("Kilix", "noun") == kilix,
                "exact default-role fallback failed");
        require(lexicon.find("KILIX", "default") == nullptr,
                "exact entry incorrectly case-folded");
        require(lexicon.find("HELLO", "default") != nullptr,
                "ascii-fold lookup failed");
        const kgv::PronunciationEntry *noun = lexicon.find("RECORD", "noun");
        const kgv::PronunciationEntry *verb = lexicon.find("record", "verb");
        require(noun != nullptr && verb != nullptr && noun != verb,
                "role-qualified lookup did not distinguish variants");
        require(noun->syllables[0U].segment_ids[1U] == 3U &&
                    verb->syllables[0U].segment_ids[1U] == 5U,
                "heteronym variants lost distinct segment IDs");
        require(lexicon.find("record", "adjective") == nullptr,
                "missing role incorrectly selected another variant");

        {
            kgv::PronunciationLexicon rejected = lexicon;
            kgv::PronunciationResourceFailure hash_failure;
            const int status = kgv::load_pronunciation_lexicon(
                resource, std::string(64U, '0'), inventory_sha256,
                kgv::PronunciationAdmission::test_fixture, segments(),
                &rejected, &hash_failure);
            require(status == KGV_HASH_MISMATCH,
                    "resource hash mismatch was accepted");
            require(hash_failure.code == "PRONUNCIATION_RESOURCE_HASH_MISMATCH",
                    "resource hash mismatch returned the wrong code");
            require(rejected.entry_count() == 0U,
                    "hash mismatch retained a previously loaded lexicon");
        }
        {
            kgv::PronunciationLexicon rejected;
            kgv::PronunciationResourceFailure inventory_failure;
            const std::string wrong_inventory(64U, 'a');
            const int status = kgv::load_pronunciation_lexicon(
                resource, kgv::sha256_hex(resource), wrong_inventory,
                kgv::PronunciationAdmission::test_fixture, segments(),
                &rejected, &inventory_failure);
            require(status == KGV_ABI_MISMATCH,
                    "segment definition hash mismatch was accepted");
            require(inventory_failure.code ==
                        "PRONUNCIATION_SEGMENT_INVENTORY_HASH_MISMATCH",
                    "segment definition mismatch returned the wrong code");
        }
        expect_failure(
            header("test-fixture", 0U, std::string(64U, 'a')),
            kgv::PronunciationAdmission::test_fixture, inventory_sha256,
            KGV_ABI_MISMATCH,
            "PRONUNCIATION_RESOURCE_INVENTORY_MISMATCH");

        expect_failure(resource, kgv::PronunciationAdmission::product_admitted,
                       inventory_sha256, KGV_INVALID_MODEL,
                       "PRONUNCIATION_RESOURCE_ADMISSION");

        expect_failure(
            header("test-fixture", 0U, inventory_sha256,
                   std::string(64U, 'a')),
            kgv::PronunciationAdmission::test_fixture, inventory_sha256,
            KGV_INVALID_MODEL, "PRONUNCIATION_RESOURCE_ADMISSION");

        const std::string one_entry = entry(
            "hello", "ascii-fold", "[\"default\"]",
            "[{\"segments\":[\"HH\"],\"stress\":\"primary\"}]");
        expect_failure(
            header("product-admitted", 1U, inventory_sha256) + one_entry,
            kgv::PronunciationAdmission::product_admitted, inventory_sha256,
            KGV_INVALID_MODEL, "PRONUNCIATION_RESOURCE_ADMISSION");
        {
            const std::string review(64U, 'a');
            const std::string product =
                header("product-admitted", 1U, inventory_sha256, review) +
                one_entry;
            kgv::PronunciationLexicon product_lexicon;
            kgv::PronunciationResourceFailure product_failure;
            require(load(product, kgv::PronunciationAdmission::product_admitted,
                         &product_lexicon, &product_failure,
                         inventory_sha256) == KGV_OK,
                    "review-bound product fixture was rejected");
            require(product_lexicon.review_record_sha256() == review,
                    "product review hash was not retained");
        }

        expect_failure(
            header("test-fixture", 1U, inventory_sha256) +
                entry("hello", "ascii-fold", "[\"default\"]",
                      "[{\"segments\":[\"ZZ\"],\"stress\":\"primary\"}]"),
            kgv::PronunciationAdmission::test_fixture, inventory_sha256,
            KGV_INVALID_MODEL, "PRONUNCIATION_UNKNOWN_SEGMENT");

        expect_failure(
            header("test-fixture", 2U, inventory_sha256) + one_entry + one_entry,
            kgv::PronunciationAdmission::test_fixture, inventory_sha256,
            KGV_INVALID_MODEL, "PRONUNCIATION_DUPLICATE_ENTRY");

        expect_failure(
            header("test-fixture", 2U, inventory_sha256) + one_entry,
            kgv::PronunciationAdmission::test_fixture, inventory_sha256,
            KGV_INVALID_MODEL, "PRONUNCIATION_RESOURCE_ENTRY_COUNT");

        expect_failure(
            header("test-fixture", 1U, inventory_sha256) +
                entry("hello", "ascii-fold", "[\"verb\",\"noun\"]",
                      "[{\"segments\":[\"HH\"],\"stress\":\"primary\"}]"),
            kgv::PronunciationAdmission::test_fixture, inventory_sha256,
            KGV_INVALID_MODEL, "PRONUNCIATION_RESOURCE_ORDER");

        expect_failure(
            header("test-fixture", 1U, inventory_sha256) +
                entry("cafe\xcc\x81", "exact", "[\"default\"]",
                      "[{\"segments\":[\"K\"],\"stress\":\"primary\"}]"),
            kgv::PronunciationAdmission::test_fixture, inventory_sha256,
            KGV_INVALID_MODEL, "PRONUNCIATION_GRAPHEME_NOT_NFC");

        expect_failure(
            header("test-fixture", 1U, inventory_sha256) +
                entry("Hello", "ascii-fold", "[\"default\"]",
                      "[{\"segments\":[\"HH\"],\"stress\":\"primary\"}]"),
            kgv::PronunciationAdmission::test_fixture, inventory_sha256,
            KGV_INVALID_MODEL, "PRONUNCIATION_INVALID_GRAPHEME");

        expect_failure(
            header("test-fixture", 1U, inventory_sha256) +
                entry("hello", "ascii-fold", "[\"default\"]",
                      "[{\"segments\":[\"HH\"],\"stress\":\"primary\"}]",
                      ",\"unknown\":true"),
            kgv::PronunciationAdmission::test_fixture, inventory_sha256,
            KGV_INVALID_MODEL, "PRONUNCIATION_RESOURCE_SCHEMA");

        {
            std::string no_final_lf = header("test-fixture", 0U, inventory_sha256);
            no_final_lf.pop_back();
            expect_failure(no_final_lf,
                           kgv::PronunciationAdmission::test_fixture,
                           inventory_sha256, KGV_INVALID_MODEL,
                           "PRONUNCIATION_RESOURCE_CANONICAL_FORM");
        }
        expect_failure(
            header("test-fixture", 0U, inventory_sha256) + "\n",
            kgv::PronunciationAdmission::test_fixture, inventory_sha256,
            KGV_INVALID_MODEL, "PRONUNCIATION_RESOURCE_CANONICAL_FORM");

        require(std::string(kgv::pronunciation_admission_name(
                    kgv::PronunciationAdmission::product_admitted)) ==
                    "product-admitted",
                "admission name changed");
        require(std::string(kgv::syllable_stress_name(
                    kgv::SyllableStress::secondary)) == "secondary",
                "stress name changed");

        std::mt19937_64 generator(0x4b47562d50524f4eULL);
        std::uniform_int_distribution<std::size_t> length_distribution(0U, 256U);
        std::uniform_int_distribution<unsigned int> byte_distribution(0U, 255U);
        for (std::size_t iteration = 0U; iteration < 2000U; ++iteration) {
            std::string bytes(length_distribution(generator), '\0');
            for (char &byte : bytes) {
                byte = static_cast<char>(byte_distribution(generator));
            }
            kgv::PronunciationLexicon fuzz_lexicon;
            kgv::PronunciationResourceFailure fuzz_failure;
            const int status = load(bytes,
                                    kgv::PronunciationAdmission::test_fixture,
                                    &fuzz_lexicon, &fuzz_failure,
                                    inventory_sha256);
            require(status != KGV_OK, "random resource unexpectedly loaded");
            require(fuzz_lexicon.entry_count() == 0U,
                    "random failed resource retained entries");
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
