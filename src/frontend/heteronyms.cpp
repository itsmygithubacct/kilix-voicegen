#include "frontend/heteronyms.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/json.h"
#include "runtime/sha256.h"

namespace kgv {
namespace {

constexpr std::size_t kMaximumResourceBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaximumLineBytes = 64U * 1024U;
constexpr std::size_t kMaximumRules = 4096U;
constexpr std::size_t kMaximumRulesPerTarget = 64U;
constexpr std::size_t kMaximumConditions = 6U;
constexpr std::size_t kMaximumConditionWords = 64U;
constexpr std::size_t kMaximumWordBytes = 64U;
constexpr int kMaximumContextDistance = 3;

class ValidationError final : public std::runtime_error {
public:
    ValidationError(std::string code, std::string message)
        : std::runtime_error(std::move(message)), code_(std::move(code)) {}

    const std::string &code() const noexcept { return code_; }

private:
    std::string code_;
};

int reject(HeteronymRules *rules,
           HeteronymResourceFailure *failure,
           int status,
           std::string code,
           std::string message,
           std::size_t line) {
    if (rules != nullptr) {
        *rules = HeteronymRules{};
    }
    if (failure != nullptr) {
        failure->status = status;
        failure->code = std::move(code);
        failure->message = std::move(message);
        failure->line = line;
    }
    return status;
}

bool exact_keys(const json::Value::Object &object,
                std::initializer_list<std::string_view> keys) {
    if (object.size() != keys.size()) {
        return false;
    }
    return std::all_of(keys.begin(), keys.end(), [&](std::string_view key) {
        return object.find(key) != object.end();
    });
}

const json::Value &required(const json::Value::Object &object,
                            std::string_view key) {
    const auto found = object.find(key);
    if (found == object.end()) {
        throw ValidationError("HETERONYM_RESOURCE_SCHEMA",
                              "required heteronym resource field is absent");
    }
    return found->second;
}

const json::Value::Object &object_value(const json::Value &value) {
    if (!value.is_object()) {
        throw ValidationError("HETERONYM_RESOURCE_SCHEMA",
                              "heteronym resource value must be an object");
    }
    return value.as_object();
}

const json::Value::Array &array_value(const json::Value &value) {
    if (!value.is_array()) {
        throw ValidationError("HETERONYM_RESOURCE_SCHEMA",
                              "heteronym resource value must be an array");
    }
    return value.as_array();
}

const std::string &string_value(const json::Value &value) {
    if (!value.is_string()) {
        throw ValidationError("HETERONYM_RESOURCE_SCHEMA",
                              "heteronym resource value must be a string");
    }
    return value.as_string();
}

std::size_t bounded_count(const json::Value &value,
                          std::size_t maximum,
                          std::string_view description) {
    std::int64_t number = 0;
    try {
        number = value.as_integer();
    } catch (const std::exception &) {
        throw ValidationError("HETERONYM_RESOURCE_SCHEMA",
                              std::string(description) + " must be an integer");
    }
    if (number < 0 || static_cast<std::uint64_t>(number) > maximum) {
        throw ValidationError("HETERONYM_RESOURCE_LIMIT",
                              std::string(description) + " exceeds its limit");
    }
    return static_cast<std::size_t>(number);
}

bool stable_identifier(std::string_view value, std::size_t maximum) noexcept {
    if (value.empty() || value.size() > maximum || value.front() < 'a' ||
        value.front() > 'z') {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '.' ||
               character == '_' || character == '-' || character == ':' ||
               character == '/';
    });
}

bool stable_role(std::string_view value) noexcept {
    if (value.empty() || value.size() > 32U || value.front() < 'a' ||
        value.front() > 'z') {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '-';
    });
}

bool canonical_word(std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaximumWordBytes ||
        value.front() < 'a' || value.front() > 'z' ||
        value.back() < 'a' || value.back() > 'z') {
        return false;
    }
    for (std::size_t index = 0U; index < value.size(); ++index) {
        const char character = value[index];
        if (character >= 'a' && character <= 'z') {
            continue;
        }
        if ((character != '\'' && character != '-') || index == 0U ||
            index + 1U == value.size() || value[index - 1U] < 'a' ||
            value[index - 1U] > 'z' || value[index + 1U] < 'a' ||
            value[index + 1U] > 'z') {
            return false;
        }
    }
    return true;
}

bool ascii_fold_word(std::string_view value, std::string *folded) {
    if (value.empty() || value.size() > kMaximumWordBytes) {
        return false;
    }
    folded->clear();
    folded->reserve(value.size());
    for (std::size_t index = 0U; index < value.size(); ++index) {
        char character = value[index];
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character + ('a' - 'A'));
        }
        if (character >= 'a' && character <= 'z') {
            folded->push_back(character);
            continue;
        }
        if ((character != '\'' && character != '-') || index == 0U ||
            index + 1U == value.size()) {
            folded->clear();
            return false;
        }
        folded->push_back(character);
    }
    return canonical_word(*folded);
}

PronunciationAdmission parse_admission(std::string_view value) {
    if (value == "product-admitted") {
        return PronunciationAdmission::product_admitted;
    }
    if (value == "test-fixture") {
        return PronunciationAdmission::test_fixture;
    }
    throw ValidationError("HETERONYM_RESOURCE_ADMISSION",
                          "heteronym rules admission is unknown or unsupported");
}

HeteronymCapitalization parse_capitalization(std::string_view value) {
    if (value == "any") return HeteronymCapitalization::any;
    if (value == "lower") return HeteronymCapitalization::lower;
    if (value == "title") return HeteronymCapitalization::title;
    if (value == "upper") return HeteronymCapitalization::upper;
    if (value == "mixed") return HeteronymCapitalization::mixed;
    throw ValidationError("HETERONYM_RESOURCE_SCHEMA",
                          "heteronym capitalization predicate is unknown");
}

HeteronymPosition parse_position(std::string_view value) {
    if (value == "any") return HeteronymPosition::any;
    if (value == "clause-start") return HeteronymPosition::clause_start;
    if (value == "clause-end") return HeteronymPosition::clause_end;
    if (value == "single-word-clause") {
        return HeteronymPosition::single_word_clause;
    }
    throw ValidationError("HETERONYM_RESOURCE_SCHEMA",
                          "heteronym clause-position predicate is unknown");
}

struct Header final {
    std::string resource_id;
    std::string base_lexicon_sha256;
    std::string review_record_sha256;
    PronunciationAdmission admission = PronunciationAdmission::test_fixture;
    std::size_t entry_count = 0U;
};

Header parse_header(const json::Value &document,
                    PronunciationAdmission required_admission,
                    std::string_view expected_base_lexicon_sha256) {
    const json::Value::Object &object = object_value(document);
    if (!exact_keys(object, {"admission", "base_lexicon_sha256", "dialect",
                             "entry_count", "resource_id",
                             "review_record_sha256", "schema"})) {
        throw ValidationError("HETERONYM_RESOURCE_SCHEMA",
                              "heteronym resource header has unknown or absent fields");
    }
    if (string_value(required(object, "schema")) !=
        "kilix.voicegen.heteronym-rules/v1") {
        throw ValidationError("HETERONYM_RESOURCE_SCHEMA",
                              "heteronym resource header schema is unsupported");
    }
    if (string_value(required(object, "dialect")) != "en-AU") {
        throw ValidationError("HETERONYM_RESOURCE_DIALECT",
                              "heteronym resource dialect must be en-AU");
    }
    Header header;
    header.admission =
        parse_admission(string_value(required(object, "admission")));
    if (header.admission != required_admission) {
        throw ValidationError("HETERONYM_RESOURCE_ADMISSION",
                              "heteronym admission does not match the caller");
    }
    header.resource_id = string_value(required(object, "resource_id"));
    if (!stable_identifier(header.resource_id, 128U)) {
        throw ValidationError("HETERONYM_RESOURCE_SCHEMA",
                              "heteronym resource ID is not canonical");
    }
    header.base_lexicon_sha256 =
        string_value(required(object, "base_lexicon_sha256"));
    if (!is_lower_sha256(header.base_lexicon_sha256)) {
        throw ValidationError("HETERONYM_RESOURCE_SCHEMA",
                              "base lexicon hash is not lowercase SHA-256");
    }
    if (header.base_lexicon_sha256 != expected_base_lexicon_sha256) {
        throw ValidationError("HETERONYM_RESOURCE_LEXICON_MISMATCH",
                              "heteronym rules target another base lexicon");
    }
    header.entry_count = bounded_count(required(object, "entry_count"),
                                       kMaximumRules,
                                       "heteronym rule count");
    if (header.admission == PronunciationAdmission::product_admitted &&
        header.entry_count == 0U) {
        throw ValidationError("HETERONYM_RESOURCE_ADMISSION",
                              "product heteronym rules cannot be empty");
    }
    const json::Value &review = required(object, "review_record_sha256");
    if (header.admission == PronunciationAdmission::product_admitted) {
        if (!review.is_string()) {
            throw ValidationError("HETERONYM_RESOURCE_ADMISSION",
                                  "product heteronym rules lack a review hash");
        }
        header.review_record_sha256 = string_value(review);
        if (!is_lower_sha256(header.review_record_sha256)) {
            throw ValidationError("HETERONYM_RESOURCE_ADMISSION",
                                  "product heteronym review hash is invalid");
        }
    } else if (!review.is_null()) {
        throw ValidationError("HETERONYM_RESOURCE_ADMISSION",
                              "test heteronym rules must not claim product review");
    }
    return header;
}

HeteronymRule parse_rule(const json::Value &document) {
    const json::Value::Object &object = object_value(document);
    if (!exact_keys(object, {"capitalization", "conditions", "position",
                             "role", "rule_id", "schema", "source",
                             "target"})) {
        throw ValidationError("HETERONYM_RESOURCE_SCHEMA",
                              "heteronym rule has unknown or absent fields");
    }
    if (string_value(required(object, "schema")) !=
        "kilix.voicegen.heteronym-rule/v1") {
        throw ValidationError("HETERONYM_RESOURCE_SCHEMA",
                              "heteronym rule schema is unsupported");
    }
    HeteronymRule rule;
    rule.rule_id = string_value(required(object, "rule_id"));
    rule.source = string_value(required(object, "source"));
    if (!stable_identifier(rule.rule_id, 128U) ||
        !stable_identifier(rule.source, 128U)) {
        throw ValidationError("HETERONYM_RESOURCE_SCHEMA",
                              "heteronym rule ID or source is not canonical");
    }
    rule.target = string_value(required(object, "target"));
    if (!canonical_word(rule.target)) {
        throw ValidationError("HETERONYM_RESOURCE_SCHEMA",
                              "heteronym target must be canonical lowercase ASCII");
    }
    rule.role = string_value(required(object, "role"));
    if (!stable_role(rule.role) || rule.role == "default") {
        throw ValidationError("HETERONYM_RESOURCE_SCHEMA",
                              "heteronym output role is not canonical and specific");
    }
    rule.capitalization = parse_capitalization(
        string_value(required(object, "capitalization")));
    rule.position =
        parse_position(string_value(required(object, "position")));

    const json::Value::Array &conditions =
        array_value(required(object, "conditions"));
    if (conditions.size() > kMaximumConditions) {
        throw ValidationError("HETERONYM_RESOURCE_LIMIT",
                              "heteronym condition count exceeds its limit");
    }
    int previous_offset = -kMaximumContextDistance - 1;
    for (const json::Value &condition_value : conditions) {
        const json::Value::Object &condition = object_value(condition_value);
        if (!exact_keys(condition, {"offset", "words"})) {
            throw ValidationError("HETERONYM_RESOURCE_SCHEMA",
                                  "heteronym condition has unknown or absent fields");
        }
        std::int64_t offset = 0;
        try {
            offset = required(condition, "offset").as_integer();
        } catch (const std::exception &) {
            throw ValidationError("HETERONYM_RESOURCE_SCHEMA",
                                  "heteronym condition offset must be an integer");
        }
        if (offset == 0 || offset < -kMaximumContextDistance ||
            offset > kMaximumContextDistance) {
            throw ValidationError("HETERONYM_RESOURCE_LIMIT",
                                  "heteronym condition offset exceeds the context window");
        }
        const int narrowed_offset = static_cast<int>(offset);
        if (narrowed_offset <= previous_offset) {
            throw ValidationError("HETERONYM_RESOURCE_ORDER",
                                  "heteronym condition offsets must be unique and sorted");
        }
        previous_offset = narrowed_offset;

        const json::Value::Array &words =
            array_value(required(condition, "words"));
        if (words.empty() || words.size() > kMaximumConditionWords) {
            throw ValidationError("HETERONYM_RESOURCE_LIMIT",
                                  "heteronym condition word count is outside its limit");
        }
        HeteronymWordCondition parsed;
        parsed.offset = narrowed_offset;
        for (const json::Value &word_value : words) {
            const std::string &word = string_value(word_value);
            if (!canonical_word(word)) {
                throw ValidationError("HETERONYM_RESOURCE_SCHEMA",
                                      "context word must be canonical lowercase ASCII");
            }
            if (!parsed.words.empty() && parsed.words.back() >= word) {
                throw ValidationError("HETERONYM_RESOURCE_ORDER",
                                      "context words must be unique and sorted");
            }
            parsed.words.push_back(word);
        }
        rule.conditions.push_back(std::move(parsed));
    }
    return rule;
}

HeteronymCapitalization classify_capitalization(std::string_view value) {
    bool all_lower = true;
    bool all_upper = true;
    bool title = true;
    bool saw_letter = false;
    bool first_letter = true;
    for (char character : value) {
        const bool lower = character >= 'a' && character <= 'z';
        const bool upper = character >= 'A' && character <= 'Z';
        if (!lower && !upper) {
            continue;
        }
        saw_letter = true;
        all_lower = all_lower && lower;
        all_upper = all_upper && upper;
        title = title && (first_letter ? upper : lower);
        first_letter = false;
    }
    if (!saw_letter || all_lower) return HeteronymCapitalization::lower;
    if (all_upper) return HeteronymCapitalization::upper;
    if (title) return HeteronymCapitalization::title;
    return HeteronymCapitalization::mixed;
}

bool matches_position(HeteronymPosition position,
                      std::size_t word_index,
                      std::size_t clause_start,
                      std::size_t clause_end) noexcept {
    switch (position) {
        case HeteronymPosition::any: return true;
        case HeteronymPosition::clause_start:
            return word_index == clause_start;
        case HeteronymPosition::clause_end:
            return word_index + 1U == clause_end;
        case HeteronymPosition::single_word_clause:
            return word_index == clause_start && word_index + 1U == clause_end;
    }
    return false;
}

bool matches_condition(const HeteronymWordCondition &condition,
                       const std::vector<LexicalWord> &words,
                       std::size_t word_index,
                       std::size_t clause_start,
                       std::size_t clause_end) {
    std::size_t neighbor = word_index;
    if (condition.offset < 0) {
        const std::size_t distance =
            static_cast<std::size_t>(-condition.offset);
        if (word_index < clause_start + distance) {
            return false;
        }
        neighbor -= distance;
    } else {
        const std::size_t distance =
            static_cast<std::size_t>(condition.offset);
        if (distance > clause_end - word_index - 1U) {
            return false;
        }
        neighbor += distance;
    }
    std::string folded;
    if (!ascii_fold_word(words[neighbor].normalized, &folded)) {
        return false;
    }
    return std::binary_search(condition.words.begin(), condition.words.end(),
                              folded);
}

}  // namespace

HeteronymDecision HeteronymRules::decide(
    const std::vector<LexicalWord> &words,
    std::size_t word_index,
    std::size_t clause_word_start,
    std::size_t clause_word_end) const {
    HeteronymDecision decision;
    if (word_index >= words.size() || clause_word_start > word_index ||
        clause_word_end <= word_index || clause_word_end > words.size()) {
        return decision;
    }
    std::string target;
    if (!ascii_fold_word(words[word_index].normalized, &target)) {
        return decision;
    }
    const auto found = index_.find(target);
    if (found == index_.end()) {
        return decision;
    }
    decision.kind = HeteronymDecisionKind::no_match;
    const HeteronymCapitalization capitalization =
        classify_capitalization(words[word_index].normalized);
    for (std::size_t rule_index : found->second) {
        const HeteronymRule &rule = rules_[rule_index];
        if (rule.capitalization != HeteronymCapitalization::any &&
            rule.capitalization != capitalization) {
            continue;
        }
        if (!matches_position(rule.position, word_index, clause_word_start,
                              clause_word_end)) {
            continue;
        }
        const bool conditions_match = std::all_of(
            rule.conditions.begin(), rule.conditions.end(),
            [&](const HeteronymWordCondition &condition) {
                return matches_condition(condition, words, word_index,
                                         clause_word_start, clause_word_end);
            });
        if (!conditions_match) {
            continue;
        }
        ++decision.match_count;
        if (decision.match_count == 1U) {
            decision.role = rule.role;
            decision.rule_id = rule.rule_id;
            decision.kind = HeteronymDecisionKind::matched;
        } else {
            decision.role.clear();
            decision.rule_id.clear();
            decision.kind = HeteronymDecisionKind::ambiguous;
        }
    }
    return decision;
}

bool HeteronymRules::contains_target(std::string_view word) const {
    std::string folded;
    return ascii_fold_word(word, &folded) && index_.find(folded) != index_.end();
}

bool HeteronymRules::compatible_with(
    const PronunciationLexicon &lexicon) const {
    return std::all_of(rules_.begin(), rules_.end(),
                       [&](const HeteronymRule &rule) {
                           return lexicon.contains_role(rule.target,
                                                        rule.role) &&
                                  lexicon.contains_role(rule.target,
                                                        "default");
                       });
}

const std::string &HeteronymRules::resource_id() const noexcept {
    return resource_id_;
}

const std::string &HeteronymRules::resource_sha256() const noexcept {
    return resource_sha256_;
}

const std::string &HeteronymRules::base_lexicon_sha256() const noexcept {
    return base_lexicon_sha256_;
}

const std::string &HeteronymRules::review_record_sha256() const noexcept {
    return review_record_sha256_;
}

PronunciationAdmission HeteronymRules::admission() const noexcept {
    return admission_;
}

std::size_t HeteronymRules::rule_count() const noexcept {
    return rules_.size();
}

int load_heteronym_rules(
    std::string_view jsonl,
    std::string_view expected_resource_sha256,
    std::string_view expected_base_lexicon_sha256,
    PronunciationAdmission required_admission,
    HeteronymRules *rules,
    HeteronymResourceFailure *failure) {
    if (rules == nullptr || failure == nullptr) {
        return KGV_INVALID_ARGUMENT;
    }
    *rules = HeteronymRules{};
    *failure = HeteronymResourceFailure{};
    if (!is_lower_sha256(expected_resource_sha256) ||
        !is_lower_sha256(expected_base_lexicon_sha256) ||
        (required_admission != PronunciationAdmission::product_admitted &&
         required_admission != PronunciationAdmission::test_fixture)) {
        return reject(rules, failure, KGV_INVALID_ARGUMENT,
                      "INVALID_HETERONYM_LOADER_ARGUMENT",
                      "heteronym loader requires pinned lowercase hashes and product or test admission",
                      0U);
    }
    if (jsonl.empty() || jsonl.size() > kMaximumResourceBytes) {
        return reject(rules, failure, KGV_RESOURCE_EXHAUSTED,
                      "HETERONYM_RESOURCE_TOO_LARGE",
                      "heteronym resource is empty or exceeds 4 MiB", 0U);
    }
    if (sha256_hex(jsonl) != expected_resource_sha256) {
        return reject(rules, failure, KGV_HASH_MISMATCH,
                      "HETERONYM_RESOURCE_HASH_MISMATCH",
                      "heteronym resource does not match its pinned SHA-256",
                      0U);
    }
    if (jsonl.back() != '\n') {
        return reject(rules, failure, KGV_INVALID_MODEL,
                      "HETERONYM_RESOURCE_CANONICAL_FORM",
                      "heteronym JSONL must end with LF", 0U);
    }

    HeteronymRules candidate;
    Header header;
    bool saw_header = false;
    std::size_t parsed_rules = 0U;
    std::size_t cursor = 0U;
    std::size_t line_number = 1U;
    std::set<std::string, std::less<>> rule_ids;
    std::string previous_target;
    std::string previous_rule_id;
    while (cursor < jsonl.size()) {
        const std::size_t end = jsonl.find('\n', cursor);
        if (end == std::string_view::npos) {
            return reject(rules, failure, KGV_INVALID_MODEL,
                          "HETERONYM_RESOURCE_CANONICAL_FORM",
                          "heteronym JSONL has an unterminated line",
                          line_number);
        }
        const std::string_view line = jsonl.substr(cursor, end - cursor);
        if (line.empty() || line.size() > kMaximumLineBytes ||
            line.find('\r') != std::string_view::npos) {
            return reject(rules, failure, KGV_INVALID_MODEL,
                          "HETERONYM_RESOURCE_CANONICAL_FORM",
                          "heteronym JSONL has an empty, oversized, or CR line",
                          line_number);
        }
        json::Value document;
        try {
            document = json::parse(line);
        } catch (const json::ParseError &) {
            return reject(rules, failure, KGV_INVALID_MODEL,
                          "HETERONYM_RESOURCE_JSON",
                          "heteronym resource line is not strict JSON",
                          line_number);
        }
        try {
            if (!saw_header) {
                header = parse_header(document, required_admission,
                                      expected_base_lexicon_sha256);
                saw_header = true;
                candidate.rules_.reserve(header.entry_count);
            } else {
                if (parsed_rules >= header.entry_count) {
                    throw ValidationError("HETERONYM_RESOURCE_ENTRY_COUNT",
                                          "heteronym resource has extra rules");
                }
                HeteronymRule rule = parse_rule(document);
                if ((!previous_target.empty() && rule.target < previous_target) ||
                    (rule.target == previous_target &&
                     rule.rule_id <= previous_rule_id)) {
                    throw ValidationError("HETERONYM_RESOURCE_ORDER",
                                          "heteronym rules must be sorted by target and rule ID");
                }
                if (!rule_ids.insert(rule.rule_id).second) {
                    throw ValidationError("HETERONYM_DUPLICATE_RULE",
                                          "heteronym rule ID is duplicated");
                }
                previous_target = rule.target;
                previous_rule_id = rule.rule_id;
                const std::size_t rule_index = candidate.rules_.size();
                std::vector<std::size_t> &target_rules =
                    candidate.index_[rule.target];
                if (target_rules.size() >= kMaximumRulesPerTarget) {
                    throw ValidationError(
                        "HETERONYM_RESOURCE_LIMIT",
                        "heteronym target exceeds its 64-rule limit");
                }
                target_rules.push_back(rule_index);
                candidate.rules_.push_back(std::move(rule));
                ++parsed_rules;
            }
        } catch (const ValidationError &error) {
            const int status =
                error.code() == "HETERONYM_RESOURCE_LEXICON_MISMATCH"
                    ? KGV_ABI_MISMATCH
                    : KGV_INVALID_MODEL;
            return reject(rules, failure, status, error.code(), error.what(),
                          line_number);
        } catch (const std::exception &) {
            return reject(rules, failure, KGV_INVALID_MODEL,
                          "HETERONYM_RESOURCE_SCHEMA",
                          "heteronym resource line has an invalid value type",
                          line_number);
        }
        cursor = end + 1U;
        ++line_number;
    }
    if (!saw_header || parsed_rules != header.entry_count) {
        return reject(rules, failure, KGV_INVALID_MODEL,
                      "HETERONYM_RESOURCE_ENTRY_COUNT",
                      "heteronym rule count does not match the header", 1U);
    }
    candidate.resource_id_ = std::move(header.resource_id);
    candidate.resource_sha256_ = std::string(expected_resource_sha256);
    candidate.base_lexicon_sha256_ =
        std::move(header.base_lexicon_sha256);
    candidate.review_record_sha256_ =
        std::move(header.review_record_sha256);
    candidate.admission_ = header.admission;
    *rules = std::move(candidate);
    return KGV_OK;
}

const char *heteronym_decision_name(HeteronymDecisionKind value) noexcept {
    switch (value) {
        case HeteronymDecisionKind::not_target: return "NOT_TARGET";
        case HeteronymDecisionKind::no_match: return "NO_MATCH";
        case HeteronymDecisionKind::matched: return "MATCHED";
        case HeteronymDecisionKind::ambiguous: return "AMBIGUOUS";
    }
    return "UNKNOWN";
}

}  // namespace kgv
