#include "frontend/heteronyms.h"

#include <cstddef>
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

std::string review_json(std::string_view review) {
    return review.empty() ? "null" : "\"" + std::string(review) + "\"";
}

std::string header(std::string_view admission,
                   std::size_t count,
                   std::string_view lexicon_sha,
                   std::string_view review = {}) {
    return "{\"admission\":\"" + std::string(admission) +
           "\",\"base_lexicon_sha256\":\"" + std::string(lexicon_sha) +
           "\",\"dialect\":\"en-AU\",\"entry_count\":" +
           std::to_string(count) +
           ",\"resource_id\":\"kilix-en-au-heteronym-test-1\"," +
           "\"review_record_sha256\":" + review_json(review) +
           ",\"schema\":\"kilix.voicegen.heteronym-rules/v1\"}\n";
}

std::string rule(std::string_view id,
                 std::string_view target,
                 std::string_view role,
                 std::string_view capitalization,
                 std::string_view position,
                 std::string_view conditions) {
    return "{\"capitalization\":\"" + std::string(capitalization) +
           "\",\"conditions\":" + std::string(conditions) +
           ",\"position\":\"" + std::string(position) +
           "\",\"role\":\"" + std::string(role) +
           "\",\"rule_id\":\"" + std::string(id) +
           "\",\"schema\":\"kilix.voicegen.heteronym-rule/v1\"," +
           "\"source\":\"project-test-fixture\",\"target\":\"" +
           std::string(target) + "\"}\n";
}

std::string valid_resource() {
    std::string resource = header("test-fixture", 3U, kLexiconSha);
    resource += rule(
        "fixture.record.after-a", "record", "noun", "any", "any",
        "[{\"offset\":-1,\"words\":[\"a\"]}]");
    resource += rule(
        "fixture.record.after-will", "record", "verb", "lower", "any",
        "[{\"offset\":-1,\"words\":[\"will\"]}]");
    resource += rule(
        "fixture.record.title-clause-start", "record", "noun", "title",
        "clause-start", "[]");
    return resource;
}

int load(std::string_view resource,
         kgv::PronunciationAdmission admission,
         kgv::HeteronymRules *rules,
         kgv::HeteronymResourceFailure *failure,
         std::string_view lexicon_sha = kLexiconSha) {
    return kgv::load_heteronym_rules(
        resource, kgv::sha256_hex(resource), lexicon_sha, admission, rules,
        failure);
}

void expect_failure(std::string resource,
                    kgv::PronunciationAdmission admission,
                    std::string_view lexicon_sha,
                    int expected_status,
                    std::string_view expected_code) {
    kgv::HeteronymRules rules;
    kgv::HeteronymResourceFailure failure;
    const int status = load(resource, admission, &rules, &failure,
                            lexicon_sha);
    require(status == expected_status && failure.status == expected_status,
            "heteronym failure returned the wrong status");
    if (failure.code != expected_code) {
        throw std::runtime_error("expected heteronym failure " +
                                 std::string(expected_code) + ", got " +
                                 failure.code);
    }
    require(rules.rule_count() == 0U && rules.resource_id().empty() &&
                rules.resource_sha256().empty() &&
                !rules.contains_target("record"),
            "failed heteronym load retained state");
}

std::vector<kgv::LexicalWord> words(
    const std::vector<std::string> &values) {
    std::vector<kgv::LexicalWord> result;
    result.reserve(values.size());
    std::size_t offset = 0U;
    for (const std::string &value : values) {
        result.push_back(kgv::LexicalWord{
            value, "WORD", kgv::SourceSpan{offset, offset + value.size()},
        });
        offset += value.size() + 1U;
    }
    return result;
}

void require_same(const kgv::HeteronymDecision &left,
                  const kgv::HeteronymDecision &right) {
    require(left.kind == right.kind && left.role == right.role &&
                left.rule_id == right.rule_id &&
                left.match_count == right.match_count,
            "heteronym decision changed across deterministic replay");
}

}  // namespace

int main() {
    try {
        const std::string resource = valid_resource();
        kgv::HeteronymRules rules;
        kgv::HeteronymResourceFailure failure;
        require(load(resource, kgv::PronunciationAdmission::test_fixture,
                     &rules, &failure) == KGV_OK,
                "valid heteronym fixture was rejected");
        require(failure.code.empty() && rules.rule_count() == 3U &&
                    rules.resource_id() == "kilix-en-au-heteronym-test-1" &&
                    rules.resource_sha256() == kgv::sha256_hex(resource) &&
                    rules.base_lexicon_sha256() == kLexiconSha &&
                    rules.review_record_sha256().empty() &&
                    rules.admission() ==
                        kgv::PronunciationAdmission::test_fixture &&
                    rules.contains_target("RECORD") &&
                    !rules.contains_target("project"),
                "heteronym resource metadata or target index changed");

        const std::vector<kgv::LexicalWord> sentence =
            words({"I", "will", "record", "a", "record"});
        const kgv::HeteronymDecision verb =
            rules.decide(sentence, 2U, 0U, sentence.size());
        const kgv::HeteronymDecision noun =
            rules.decide(sentence, 4U, 0U, sentence.size());
        require(verb.kind == kgv::HeteronymDecisionKind::matched &&
                    verb.role == "verb" &&
                    verb.rule_id == "fixture.record.after-will" &&
                    verb.match_count == 1U,
                "bounded previous-word rule did not select the verb");
        require(noun.kind == kgv::HeteronymDecisionKind::matched &&
                    noun.role == "noun" &&
                    noun.rule_id == "fixture.record.after-a" &&
                    noun.match_count == 1U,
                "bounded previous-word rule did not select the noun");

        const std::vector<kgv::LexicalWord> titled = words({"Record"});
        const kgv::HeteronymDecision titled_noun =
            rules.decide(titled, 0U, 0U, 1U);
        require(titled_noun.kind == kgv::HeteronymDecisionKind::matched &&
                    titled_noun.role == "noun" &&
                    titled_noun.rule_id ==
                        "fixture.record.title-clause-start",
                "capitalization and clause-start predicates changed");

        const std::vector<kgv::LexicalWord> lower = words({"record"});
        require(rules.decide(lower, 0U, 0U, 1U).kind ==
                    kgv::HeteronymDecisionKind::no_match,
                "unmatched target did not request a documented default");
        const std::vector<kgv::LexicalWord> separated =
            words({"will", "record"});
        require(rules.decide(separated, 1U, 1U, 2U).kind ==
                    kgv::HeteronymDecisionKind::no_match,
                "context rule crossed a clause boundary");
        require(rules.decide(sentence, 0U, 0U, sentence.size()).kind ==
                    kgv::HeteronymDecisionKind::not_target &&
                    rules.decide(sentence, sentence.size(), 0U,
                                 sentence.size()).kind ==
                        kgv::HeteronymDecisionKind::not_target,
                "non-target or invalid index produced a role");

        std::string ambiguous = header("test-fixture", 4U, kLexiconSha);
        ambiguous += rule(
            "fixture.record.after-a", "record", "noun", "any", "any",
            "[{\"offset\":-1,\"words\":[\"a\"]}]");
        ambiguous += rule(
            "fixture.record.after-will", "record", "verb", "lower", "any",
            "[{\"offset\":-1,\"words\":[\"will\"]}]");
        ambiguous += rule("fixture.record.any-lower", "record", "noun",
                          "lower", "any", "[]");
        ambiguous += rule(
            "fixture.record.title-clause-start", "record", "noun", "title",
            "clause-start", "[]");
        kgv::HeteronymRules ambiguous_rules;
        require(load(ambiguous, kgv::PronunciationAdmission::test_fixture,
                     &ambiguous_rules, &failure) == KGV_OK,
                "overlapping synthetic rule fixture did not load");
        const kgv::HeteronymDecision ambiguous_decision =
            ambiguous_rules.decide(sentence, 2U, 0U, sentence.size());
        require(ambiguous_decision.kind ==
                    kgv::HeteronymDecisionKind::ambiguous &&
                    ambiguous_decision.match_count == 2U &&
                    ambiguous_decision.role.empty() &&
                    ambiguous_decision.rule_id.empty(),
                "overlapping contextual rules did not fail closed");
        require(std::string(kgv::heteronym_decision_name(
                    ambiguous_decision.kind)) == "AMBIGUOUS",
                "heteronym decision name changed");

        {
            kgv::HeteronymRules rejected = rules;
            kgv::HeteronymResourceFailure hash_failure;
            const int status = kgv::load_heteronym_rules(
                resource, std::string(64U, '0'), kLexiconSha,
                kgv::PronunciationAdmission::test_fixture, &rejected,
                &hash_failure);
            require(status == KGV_HASH_MISMATCH &&
                        hash_failure.code ==
                            "HETERONYM_RESOURCE_HASH_MISMATCH" &&
                        rejected.rule_count() == 0U &&
                        !rejected.contains_target("record"),
                    "heteronym hash mismatch retained loaded state");
        }

        expect_failure(resource,
                       kgv::PronunciationAdmission::product_admitted,
                       kLexiconSha, KGV_INVALID_MODEL,
                       "HETERONYM_RESOURCE_ADMISSION");
        expect_failure(resource,
                       kgv::PronunciationAdmission::test_fixture,
                       std::string(64U, 'b'), KGV_ABI_MISMATCH,
                       "HETERONYM_RESOURCE_LEXICON_MISMATCH");
        expect_failure(
            header("product-admitted", 1U, kLexiconSha) +
                rule("fixture.record.any", "record", "noun", "any", "any",
                     "[]"),
            kgv::PronunciationAdmission::product_admitted, kLexiconSha,
            KGV_INVALID_MODEL, "HETERONYM_RESOURCE_ADMISSION");
        {
            const std::string review(64U, 'c');
            const std::string product =
                header("product-admitted", 1U, kLexiconSha, review) +
                rule("fixture.record.any", "record", "noun", "any", "any",
                     "[]");
            kgv::HeteronymRules product_rules;
            require(load(product,
                         kgv::PronunciationAdmission::product_admitted,
                         &product_rules, &failure) == KGV_OK &&
                        product_rules.review_record_sha256() == review,
                    "review-bound product heteronym fixture was rejected");
        }
        expect_failure(
            header("test-fixture", 1U, kLexiconSha) +
                rule("fixture.record.zero", "record", "noun", "any", "any",
                     "[{\"offset\":0,\"words\":[\"will\"]}]"),
            kgv::PronunciationAdmission::test_fixture, kLexiconSha,
            KGV_INVALID_MODEL, "HETERONYM_RESOURCE_LIMIT");
        expect_failure(
            header("test-fixture", 2U, kLexiconSha) +
                rule("fixture.shared", "cat", "noun", "any", "any", "[]") +
                rule("fixture.shared", "record", "verb", "any", "any",
                     "[]"),
            kgv::PronunciationAdmission::test_fixture, kLexiconSha,
            KGV_INVALID_MODEL, "HETERONYM_DUPLICATE_RULE");
        expect_failure(
            header("test-fixture", 2U, kLexiconSha) +
                rule("fixture.record.z", "record", "verb", "any", "any",
                     "[]") +
                rule("fixture.record.a", "record", "noun", "any", "any",
                     "[]"),
            kgv::PronunciationAdmission::test_fixture, kLexiconSha,
            KGV_INVALID_MODEL, "HETERONYM_RESOURCE_ORDER");
        {
            std::string excessive =
                header("test-fixture", 65U, kLexiconSha);
            for (std::size_t index = 0U; index < 65U; ++index) {
                const std::string suffix =
                    (index < 10U ? "0" : "") + std::to_string(index);
                excessive += rule("fixture.record.limit-" + suffix,
                                  "record", "noun", "any", "any", "[]");
            }
            expect_failure(excessive,
                           kgv::PronunciationAdmission::test_fixture,
                           kLexiconSha, KGV_INVALID_MODEL,
                           "HETERONYM_RESOURCE_LIMIT");
        }
        expect_failure(resource.substr(0U, resource.size() - 1U),
                       kgv::PronunciationAdmission::test_fixture,
                       kLexiconSha, KGV_INVALID_MODEL,
                       "HETERONYM_RESOURCE_CANONICAL_FORM");

        std::mt19937_64 generator(0x4b47562d48455445ULL);
        const std::vector<std::string> previous = {"will", "a", "the", "x"};
        const std::vector<std::string> targets = {"record", "Record", "RECORD"};
        for (std::size_t iteration = 0U; iteration < 2000U; ++iteration) {
            const std::string &before =
                previous[static_cast<std::size_t>(generator() % previous.size())];
            const std::string &target =
                targets[static_cast<std::size_t>(generator() % targets.size())];
            const std::vector<kgv::LexicalWord> request =
                words({before, target});
            const kgv::HeteronymDecision first =
                rules.decide(request, 1U, 0U, 2U);
            const kgv::HeteronymDecision second =
                rules.decide(request, 1U, 0U, 2U);
            require_same(first, second);
        }

        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
