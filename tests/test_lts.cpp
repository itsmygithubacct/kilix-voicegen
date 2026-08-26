#include "frontend/lts.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
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
        {"AO", 1U}, {"AE", 2U}, {"B", 3U}, {"D", 4U}, {"K", 5U},
        {"M", 6U},  {"N", 7U},  {"P", 8U}, {"T", 9U},
    };
    return value;
}

std::string review_json(std::string_view review) {
    return review.empty() ? "null" : "\"" + std::string(review) + "\"";
}

std::string header(std::string_view admission,
                   std::size_t node_count,
                   std::string_view roots,
                   std::string_view inventory_sha256,
                   std::size_t maximum_steps = 2U,
                   std::string_view review = {}) {
    return "{\"admission\":\"" + std::string(admission) +
           "\",\"context_left\":1,\"context_right\":1,\"dialect\":\"en-AU\"," +
           "\"maximum_steps\":" + std::to_string(maximum_steps) +
           ",\"node_count\":" + std::to_string(node_count) +
           ",\"resource_id\":\"kilix-en-au-lts-test-1\"," +
           "\"review_record_sha256\":" + review_json(review) +
           ",\"roots\":" + std::string(roots) +
           ",\"schema\":\"kilix.voicegen.lts-model/v1\"," +
           "\"segment_inventory_sha256\":\"" +
           std::string(inventory_sha256) +
           "\",\"source_lexicon_sha256\":\"" + std::string(64U, '1') +
           "\",\"training_record_sha256\":\"" + std::string(64U, '2') +
           "\"}\n";
}

std::string decision(std::size_t id,
                     int offset,
                     char value,
                     std::size_t yes,
                     std::size_t no) {
    return "{\"feature_offset\":" + std::to_string(offset) +
           ",\"feature_value\":\"" + std::string(1U, value) +
           "\",\"id\":" + std::to_string(id) +
           ",\"kind\":\"decision\",\"no\":" + std::to_string(no) +
           ",\"schema\":\"kilix.voicegen.lts-node/v1\",\"yes\":" +
           std::to_string(yes) + "}\n";
}

std::string leaf(std::size_t id, std::string_view emissions) {
    return "{\"emissions\":" + std::string(emissions) +
           ",\"id\":" + std::to_string(id) +
           ",\"kind\":\"leaf\",\"schema\":\"kilix.voicegen.lts-node/v1\"}\n";
}

std::string valid_fixture(std::string_view inventory_sha256) {
    std::string resource = header(
        "test-fixture", 7U, "{\"a\":1,\"b\":5,\"c\":0,\"e\":6,\"t\":4}",
        inventory_sha256);
    resource += leaf(0U, "[{\"segment_ids\":[5],\"syllable_end\":null}]");
    resource += decision(1U, 1, 't', 2U, 3U);
    resource += leaf(2U, "[{\"segment_ids\":[2],\"syllable_end\":null}]");
    resource += leaf(3U, "[{\"segment_ids\":[1],\"syllable_end\":null}]");
    resource += leaf(
        4U, "[{\"segment_ids\":[9],\"syllable_end\":\"primary\"}]");
    resource += leaf(
        5U, "[{\"segment_ids\":[3],\"syllable_end\":\"primary\"}]");
    resource += leaf(6U, "[]");
    return resource;
}

int load(std::string_view resource,
         kgv::PronunciationAdmission admission,
         std::string_view inventory_sha256,
         kgv::LtsModel *model,
         kgv::LtsFailure *failure) {
    return kgv::load_lts_model(resource, kgv::sha256_hex(resource),
                               inventory_sha256, admission, segments(), model,
                               failure);
}

void expect_failure(std::string resource,
                    kgv::PronunciationAdmission admission,
                    std::string_view inventory_sha256,
                    int expected_status,
                    std::string_view expected_code) {
    kgv::LtsModel model;
    kgv::LtsFailure failure;
    const int status = load(resource, admission, inventory_sha256, &model,
                            &failure);
    require(status == expected_status, "LTS failure returned wrong status");
    require(failure.status == status, "LTS failure record status differs");
    if (failure.code != expected_code) {
        throw std::runtime_error("expected LTS failure code " +
                                 std::string(expected_code) + ", got " +
                                 failure.code);
    }
    require(model.node_count() == 0U, "failed LTS load retained nodes");
    require(model.resource_id().empty(), "failed LTS load retained metadata");
}

std::string all_letter_roots() {
    std::string roots = "{";
    for (char symbol = 'a'; symbol <= 'z'; ++symbol) {
        if (symbol != 'a') roots.push_back(',');
        roots += "\"" + std::string(1U, symbol) + "\":0";
    }
    roots.push_back('}');
    return roots;
}

}  // namespace

int main(int argc, char **argv) {
    try {
        const std::string inventory_sha256 =
            kgv::pronunciation_segment_inventory_sha256(segments());
        require(inventory_sha256.size() == 64U,
                "LTS test inventory hash was not produced");
        if (argc == 3 &&
            (std::string_view(argv[1]) == "--validate" ||
             std::string_view(argv[1]) == "--validate-product")) {
            const bool product =
                std::string_view(argv[1]) == "--validate-product";
            std::ifstream stream(argv[2], std::ios::binary);
            require(stream.good(), "could not open generated LTS model");
            const std::string generated((std::istreambuf_iterator<char>(stream)),
                                        std::istreambuf_iterator<char>());
            kgv::LtsModel generated_model;
            kgv::LtsFailure generated_failure;
            require(load(generated,
                         product
                             ? kgv::PronunciationAdmission::product_admitted
                             : kgv::PronunciationAdmission::test_fixture,
                         inventory_sha256, &generated_model,
                         &generated_failure) == KGV_OK,
                    "generated LTS model failed native validation");
            require(generated_model.node_count() > 0U &&
                        generated_model.root_count() > 0U,
                    "generated LTS model is empty");
            const std::vector<std::string_view> words =
                product ? std::vector<std::string_view>{"aa", "az", "za", "zz"}
                        : std::vector<std::string_view>{"bat", "cab", "cat", "tan"};
            for (std::string_view word : words) {
                std::vector<kgv::PronunciationSyllable> generated_pronunciation;
                require(generated_model.pronounce(
                            word, &generated_pronunciation,
                            &generated_failure) == KGV_OK &&
                            !generated_pronunciation.empty(),
                        "generated LTS model failed native inference");
            }
            return 0;
        }
        require(argc == 1, "unknown LTS test arguments");
        const std::string resource = valid_fixture(inventory_sha256);

        kgv::LtsModel model;
        kgv::LtsFailure failure;
        require(load(resource, kgv::PronunciationAdmission::test_fixture,
                     inventory_sha256, &model, &failure) == KGV_OK,
                "valid LTS fixture was rejected");
        require(failure.code.empty(), "valid LTS load retained a failure");
        require(model.node_count() == 7U && model.root_count() == 5U,
                "LTS graph dimensions changed");
        require(model.resource_id() == "kilix-en-au-lts-test-1",
                "LTS resource ID changed");
        require(model.resource_sha256() == kgv::sha256_hex(resource),
                "LTS resource hash was not retained");
        require(model.segment_inventory_sha256() == inventory_sha256,
                "LTS inventory hash was not retained");
        require(model.source_lexicon_sha256() == std::string(64U, '1') &&
                    model.training_record_sha256() == std::string(64U, '2'),
                "LTS training bindings changed");
        require(model.review_record_sha256().empty(),
                "test LTS model acquired a review binding");

        std::vector<kgv::PronunciationSyllable> pronunciation;
        require(model.pronounce("cat", &pronunciation, &failure) == KGV_OK,
                "context-sensitive cat pronunciation failed");
        require(pronunciation.size() == 1U &&
                    pronunciation[0U].stress == kgv::SyllableStress::primary &&
                    pronunciation[0U].segment_ids ==
                        std::vector<std::uint16_t>({5U, 2U, 9U}),
                "cat LTS output changed");
        require(model.pronounce("cab", &pronunciation, &failure) == KGV_OK,
                "context-sensitive cab pronunciation failed");
        require(pronunciation[0U].segment_ids ==
                    std::vector<std::uint16_t>({5U, 1U, 3U}),
                "right-context LTS branch did not change the vowel");
        require(model.pronounce("cate", &pronunciation, &failure) == KGV_OK,
                "silent final letter was not supported");
        require(pronunciation[0U].segment_ids ==
                    std::vector<std::uint16_t>({5U, 2U, 9U}),
                "silent letter changed the emitted pronunciation");

        kgv::PronunciationSyllable sentinel;
        sentinel.stress = kgv::SyllableStress::primary;
        sentinel.segment_ids = {5U};
        pronunciation = {sentinel};
        require(model.pronounce("Cat", &pronunciation, &failure) ==
                    KGV_INVALID_TEXT &&
                    failure.code == "LTS_INVALID_GRAPHEME" &&
                    pronunciation.empty(),
                "noncanonical LTS input did not fail closed");
        require(model.pronounce("dad", &pronunciation, &failure) ==
                    KGV_INVALID_TEXT &&
                    failure.code == "LTS_UNSUPPORTED_GRAPHEME",
                "missing LTS root did not fail explicitly");
        require(model.pronounce("ace", &pronunciation, &failure) ==
                    KGV_INVALID_MODEL &&
                    failure.code == "LTS_INVALID_EMISSION_SEQUENCE",
                "open LTS syllable was accepted");
        require(model.pronounce("c''at", &pronunciation, &failure) ==
                    KGV_INVALID_TEXT &&
                    failure.code == "LTS_INVALID_GRAPHEME",
                "noncanonical internal punctuation was accepted");

        {
            kgv::LtsModel rejected = model;
            kgv::LtsFailure hash_failure;
            const int status = kgv::load_lts_model(
                resource, std::string(64U, '0'), inventory_sha256,
                kgv::PronunciationAdmission::test_fixture, segments(),
                &rejected, &hash_failure);
            require(status == KGV_HASH_MISMATCH &&
                        hash_failure.code == "LTS_RESOURCE_HASH_MISMATCH" &&
                        rejected.node_count() == 0U,
                    "LTS resource hash mismatch did not clear state");
        }
        {
            kgv::LtsModel rejected;
            kgv::LtsFailure inventory_failure;
            const int status = kgv::load_lts_model(
                resource, kgv::sha256_hex(resource), std::string(64U, 'a'),
                kgv::PronunciationAdmission::test_fixture, segments(),
                &rejected, &inventory_failure);
            require(status == KGV_ABI_MISMATCH &&
                        inventory_failure.code ==
                            "LTS_SEGMENT_INVENTORY_HASH_MISMATCH",
                    "LTS supplied inventory mismatch was accepted");
        }
        expect_failure(
            header("test-fixture", 1U, "{\"c\":0}", std::string(64U, 'a'),
                   1U) +
                leaf(0U, "[]"),
            kgv::PronunciationAdmission::test_fixture, inventory_sha256,
            KGV_ABI_MISMATCH, "LTS_RESOURCE_INVENTORY_MISMATCH");

        expect_failure(resource,
                       kgv::PronunciationAdmission::product_admitted,
                       inventory_sha256, KGV_INVALID_MODEL,
                       "LTS_RESOURCE_ADMISSION");
        expect_failure(
            header("test-fixture", 1U, "{\"c\":0}", inventory_sha256, 1U,
                   std::string(64U, 'a')) +
                leaf(0U, "[]"),
            kgv::PronunciationAdmission::test_fixture, inventory_sha256,
            KGV_INVALID_MODEL, "LTS_RESOURCE_ADMISSION");

        {
            const std::string product =
                header("product-admitted", 1U, all_letter_roots(),
                       inventory_sha256, 1U, std::string(64U, 'a')) +
                leaf(0U,
                     "[{\"segment_ids\":[5],\"syllable_end\":\"primary\"}]");
            kgv::LtsModel product_model;
            kgv::LtsFailure product_failure;
            require(load(product,
                         kgv::PronunciationAdmission::product_admitted,
                         inventory_sha256, &product_model,
                         &product_failure) == KGV_OK &&
                        product_model.review_record_sha256() ==
                            std::string(64U, 'a'),
                    "review-bound product LTS fixture was rejected");
        }

        expect_failure(
            header("test-fixture", 1U, "{\"c\":0}", inventory_sha256, 2U) +
                decision(0U, 1, '$', 0U, 0U),
            kgv::PronunciationAdmission::test_fixture, inventory_sha256,
            KGV_INVALID_MODEL, "LTS_RESOURCE_GRAPH");
        expect_failure(
            header("test-fixture", 2U, "{\"c\":0}", inventory_sha256, 1U) +
                leaf(0U, "[]") + leaf(1U, "[]"),
            kgv::PronunciationAdmission::test_fixture, inventory_sha256,
            KGV_INVALID_MODEL, "LTS_RESOURCE_GRAPH");
        expect_failure(
            header("test-fixture", 2U, "{\"c\":0}", inventory_sha256, 1U) +
                leaf(0U, "[]"),
            kgv::PronunciationAdmission::test_fixture, inventory_sha256,
            KGV_INVALID_MODEL, "LTS_RESOURCE_NODE_COUNT");
        expect_failure(
            header("test-fixture", 2U, "{\"c\":0}", inventory_sha256, 1U) +
                leaf(0U, "[]") + leaf(0U, "[]"),
            kgv::PronunciationAdmission::test_fixture, inventory_sha256,
            KGV_INVALID_MODEL, "LTS_RESOURCE_ORDER");
        expect_failure(
            header("test-fixture", 3U, "{\"c\":0}", inventory_sha256, 2U) +
                decision(0U, 1, '$', 1U, 1U) +
                decision(1U, 1, '$', 2U, 2U) + leaf(2U, "[]"),
            kgv::PronunciationAdmission::test_fixture, inventory_sha256,
            KGV_INVALID_MODEL, "LTS_RESOURCE_GRAPH");
        expect_failure(
            header("test-fixture", 2U, "{\"c\":0}", inventory_sha256, 2U) +
                decision(0U, 2, '$', 1U, 1U) + leaf(1U, "[]"),
            kgv::PronunciationAdmission::test_fixture, inventory_sha256,
            KGV_INVALID_MODEL, "LTS_RESOURCE_SCHEMA");
        expect_failure(
            header("test-fixture", 1U, "{\"c\":0}", inventory_sha256, 1U) +
                leaf(0U,
                     "[{\"segment_ids\":[99],\"syllable_end\":\"primary\"}]"),
            kgv::PronunciationAdmission::test_fixture, inventory_sha256,
            KGV_INVALID_MODEL, "LTS_UNKNOWN_SEGMENT");

        {
            std::string unknown_field = leaf(0U, "[]");
            unknown_field.insert(unknown_field.size() - 2U,
                                 ",\"unknown\":true");
            expect_failure(
                header("test-fixture", 1U, "{\"c\":0}", inventory_sha256,
                       1U) +
                    unknown_field,
                kgv::PronunciationAdmission::test_fixture, inventory_sha256,
                KGV_INVALID_MODEL, "LTS_RESOURCE_SCHEMA");
        }
        {
            const std::string repeated_primary =
                header("test-fixture", 1U, "{\"c\":0}", inventory_sha256,
                       1U) +
                leaf(0U,
                     "[{\"segment_ids\":[5],\"syllable_end\":\"primary\"}]");
            kgv::LtsModel repeated_model;
            kgv::LtsFailure repeated_failure;
            require(load(repeated_primary,
                         kgv::PronunciationAdmission::test_fixture,
                         inventory_sha256, &repeated_model,
                         &repeated_failure) == KGV_OK,
                    "valid repeated-emission fixture failed to load");
            require(repeated_model.pronounce(
                        "cc", &pronunciation, &repeated_failure) ==
                        KGV_INVALID_MODEL &&
                        repeated_failure.code ==
                            "LTS_INVALID_EMISSION_SEQUENCE" &&
                        pronunciation.empty(),
                    "multiple predicted primary stresses were accepted");
        }

        {
            std::string no_final_lf =
                header("test-fixture", 1U, "{\"c\":0}", inventory_sha256,
                       1U) +
                leaf(0U, "[]");
            no_final_lf.pop_back();
            expect_failure(no_final_lf,
                           kgv::PronunciationAdmission::test_fixture,
                           inventory_sha256, KGV_INVALID_MODEL,
                           "LTS_RESOURCE_CANONICAL_FORM");
        }

        std::mt19937_64 generator(0x4b47562d4c545331ULL);
        std::uniform_int_distribution<std::size_t> length_distribution(0U,
                                                                       256U);
        std::uniform_int_distribution<unsigned int> byte_distribution(0U,
                                                                       255U);
        for (std::size_t iteration = 0U; iteration < 2000U; ++iteration) {
            std::string bytes(length_distribution(generator), '\0');
            for (char &byte : bytes) {
                byte = static_cast<char>(byte_distribution(generator));
            }
            kgv::LtsModel fuzz_model;
            kgv::LtsFailure fuzz_failure;
            const int status = load(bytes,
                                    kgv::PronunciationAdmission::test_fixture,
                                    inventory_sha256, &fuzz_model,
                                    &fuzz_failure);
            require(status != KGV_OK, "random LTS resource unexpectedly loaded");
            require(fuzz_model.node_count() == 0U,
                    "random failed LTS resource retained nodes");
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
