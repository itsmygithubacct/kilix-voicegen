#include "frontend/weak_forms.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
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
constexpr std::size_t kMaximumVowels = 64U;
constexpr std::size_t kMaximumWordBytes = 64U;

class ValidationError final : public std::runtime_error {
public:
    ValidationError(std::string code, std::string message)
        : std::runtime_error(std::move(message)), code_(std::move(code)) {}

    const std::string &code() const noexcept { return code_; }

private:
    std::string code_;
};

int reject(WeakFormRules *rules,
           WeakFormResourceFailure *failure,
           int status,
           std::string code,
           std::string message,
           std::size_t line) {
    if (rules != nullptr) {
        *rules = WeakFormRules{};
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
        throw ValidationError("WEAK_FORM_RESOURCE_SCHEMA",
                              "required weak-form resource field is absent");
    }
    return found->second;
}

const json::Value::Object &object_value(const json::Value &value) {
    if (!value.is_object()) {
        throw ValidationError("WEAK_FORM_RESOURCE_SCHEMA",
                              "weak-form resource value must be an object");
    }
    return value.as_object();
}

const json::Value::Array &array_value(const json::Value &value) {
    if (!value.is_array()) {
        throw ValidationError("WEAK_FORM_RESOURCE_SCHEMA",
                              "weak-form resource value must be an array");
    }
    return value.as_array();
}

const std::string &string_value(const json::Value &value) {
    if (!value.is_string()) {
        throw ValidationError("WEAK_FORM_RESOURCE_SCHEMA",
                              "weak-form resource value must be a string");
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
        throw ValidationError("WEAK_FORM_RESOURCE_SCHEMA",
                              std::string(description) + " must be an integer");
    }
    if (number < 0 || static_cast<std::uint64_t>(number) > maximum) {
        throw ValidationError("WEAK_FORM_RESOURCE_LIMIT",
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
        value.front() < 'a' || value.front() > 'z' || value.back() < 'a' ||
        value.back() > 'z') {
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
    throw ValidationError("WEAK_FORM_RESOURCE_ADMISSION",
                          "weak-form admission is unknown or unsupported");
}

WeakFormCapitalization parse_capitalization(std::string_view value) {
    if (value == "any") return WeakFormCapitalization::any;
    if (value == "lower") return WeakFormCapitalization::lower;
    if (value == "title") return WeakFormCapitalization::title;
    if (value == "upper") return WeakFormCapitalization::upper;
    if (value == "mixed") return WeakFormCapitalization::mixed;
    throw ValidationError("WEAK_FORM_RESOURCE_SCHEMA",
                          "weak-form capitalization predicate is unknown");
}

WeakFormPosition parse_position(std::string_view value) {
    if (value == "any") return WeakFormPosition::any;
    if (value == "phrase-medial") return WeakFormPosition::phrase_medial;
    if (value == "phrase-final") return WeakFormPosition::phrase_final;
    if (value == "single-word-phrase") {
        return WeakFormPosition::single_word_phrase;
    }
    throw ValidationError("WEAK_FORM_RESOURCE_SCHEMA",
                          "weak-form phrase-position predicate is unknown");
}

WeakFormNextSegment parse_next_segment(std::string_view value) {
    if (value == "any") return WeakFormNextSegment::any;
    if (value == "vowel") return WeakFormNextSegment::vowel;
    if (value == "non-vowel") return WeakFormNextSegment::non_vowel;
    if (value == "absent") return WeakFormNextSegment::absent;
    throw ValidationError("WEAK_FORM_RESOURCE_SCHEMA",
                          "weak-form next-segment predicate is unknown");
}

struct Header final {
    std::string resource_id;
    std::string base_lexicon_sha256;
    std::string segment_inventory_sha256;
    std::string review_record_sha256;
    PronunciationAdmission admission = PronunciationAdmission::test_fixture;
    std::size_t entry_count = 0U;
    std::vector<std::uint16_t> vowel_segment_ids;
};

Header parse_header(
    const json::Value &document,
    PronunciationAdmission required_admission,
    std::string_view expected_base_lexicon_sha256,
    std::string_view expected_segment_inventory_sha256,
    const std::map<std::string, std::uint16_t, std::less<>> &segment_ids) {
    const json::Value::Object &object = object_value(document);
    if (!exact_keys(object,
                    {"admission", "base_lexicon_sha256", "dialect",
                     "entry_count", "resource_id", "review_record_sha256",
                     "schema", "segment_inventory_sha256",
                     "vowel_segments"})) {
        throw ValidationError(
            "WEAK_FORM_RESOURCE_SCHEMA",
            "weak-form resource header has unknown or absent fields");
    }
    if (string_value(required(object, "schema")) !=
        "kilix.voicegen.weak-form-rules/v1") {
        throw ValidationError("WEAK_FORM_RESOURCE_SCHEMA",
                              "weak-form resource header schema is unsupported");
    }
    if (string_value(required(object, "dialect")) != "en-AU") {
        throw ValidationError("WEAK_FORM_RESOURCE_DIALECT",
                              "weak-form resource dialect must be en-AU");
    }

    Header header;
    header.admission =
        parse_admission(string_value(required(object, "admission")));
    if (header.admission != required_admission) {
        throw ValidationError("WEAK_FORM_RESOURCE_ADMISSION",
                              "weak-form admission does not match the caller");
    }
    header.resource_id = string_value(required(object, "resource_id"));
    if (!stable_identifier(header.resource_id, 128U)) {
        throw ValidationError("WEAK_FORM_RESOURCE_SCHEMA",
                              "weak-form resource ID is not canonical");
    }
    header.base_lexicon_sha256 =
        string_value(required(object, "base_lexicon_sha256"));
    if (!is_lower_sha256(header.base_lexicon_sha256)) {
        throw ValidationError("WEAK_FORM_RESOURCE_SCHEMA",
                              "base lexicon hash is not lowercase SHA-256");
    }
    if (header.base_lexicon_sha256 != expected_base_lexicon_sha256) {
        throw ValidationError("WEAK_FORM_RESOURCE_LEXICON_MISMATCH",
                              "weak-form rules target another base lexicon");
    }
    header.segment_inventory_sha256 =
        string_value(required(object, "segment_inventory_sha256"));
    if (!is_lower_sha256(header.segment_inventory_sha256)) {
        throw ValidationError("WEAK_FORM_RESOURCE_SCHEMA",
                              "segment inventory hash is not lowercase SHA-256");
    }
    if (header.segment_inventory_sha256 !=
        expected_segment_inventory_sha256) {
        throw ValidationError("WEAK_FORM_RESOURCE_INVENTORY_MISMATCH",
                              "weak-form rules target another segment inventory");
    }
    header.entry_count = bounded_count(required(object, "entry_count"),
                                       kMaximumRules,
                                       "weak-form rule count");
    if (header.admission == PronunciationAdmission::product_admitted &&
        header.entry_count == 0U) {
        throw ValidationError("WEAK_FORM_RESOURCE_ADMISSION",
                              "product weak-form rules cannot be empty");
    }

    const json::Value &review = required(object, "review_record_sha256");
    if (header.admission == PronunciationAdmission::product_admitted) {
        if (!review.is_string()) {
            throw ValidationError("WEAK_FORM_RESOURCE_ADMISSION",
                                  "product weak-form rules lack a review hash");
        }
        header.review_record_sha256 = string_value(review);
        if (!is_lower_sha256(header.review_record_sha256)) {
            throw ValidationError("WEAK_FORM_RESOURCE_ADMISSION",
                                  "product weak-form review hash is invalid");
        }
    } else if (!review.is_null()) {
        throw ValidationError("WEAK_FORM_RESOURCE_ADMISSION",
                              "test weak-form rules must not claim product review");
    }

    const json::Value::Array &vowels =
        array_value(required(object, "vowel_segments"));
    if (vowels.empty() || vowels.size() > kMaximumVowels ||
        vowels.size() >= segment_ids.size()) {
        throw ValidationError(
            "WEAK_FORM_RESOURCE_LIMIT",
            "weak-form vowel class must leave non-vowel inventory members");
    }
    std::uint16_t previous_id = 0U;
    for (const json::Value &value : vowels) {
        const std::string &name = string_value(value);
        const auto found = segment_ids.find(name);
        if (found == segment_ids.end()) {
            throw ValidationError("WEAK_FORM_UNKNOWN_SEGMENT",
                                  "weak-form vowel class names an unknown segment");
        }
        if (!header.vowel_segment_ids.empty() && found->second <= previous_id) {
            throw ValidationError(
                "WEAK_FORM_RESOURCE_ORDER",
                "weak-form vowel segments must be unique and sorted by inventory ID");
        }
        previous_id = found->second;
        header.vowel_segment_ids.push_back(found->second);
    }
    return header;
}

WeakFormRule parse_rule(const json::Value &document) {
    const json::Value::Object &object = object_value(document);
    if (!exact_keys(object,
                    {"capitalization", "next_segment", "position", "role",
                     "rule_id", "schema", "source", "target"})) {
        throw ValidationError("WEAK_FORM_RESOURCE_SCHEMA",
                              "weak-form rule has unknown or absent fields");
    }
    if (string_value(required(object, "schema")) !=
        "kilix.voicegen.weak-form-rule/v1") {
        throw ValidationError("WEAK_FORM_RESOURCE_SCHEMA",
                              "weak-form rule schema is unsupported");
    }

    WeakFormRule rule;
    rule.rule_id = string_value(required(object, "rule_id"));
    rule.source = string_value(required(object, "source"));
    if (!stable_identifier(rule.rule_id, 128U) ||
        !stable_identifier(rule.source, 128U)) {
        throw ValidationError("WEAK_FORM_RESOURCE_SCHEMA",
                              "weak-form rule ID or source is not canonical");
    }
    rule.target = string_value(required(object, "target"));
    if (!canonical_word(rule.target)) {
        throw ValidationError(
            "WEAK_FORM_RESOURCE_SCHEMA",
            "weak-form target must be canonical lowercase ASCII");
    }
    rule.role = string_value(required(object, "role"));
    if (!stable_role(rule.role) || rule.role == "default") {
        throw ValidationError(
            "WEAK_FORM_RESOURCE_SCHEMA",
            "weak-form output role is not canonical and specific");
    }
    rule.capitalization = parse_capitalization(
        string_value(required(object, "capitalization")));
    rule.position = parse_position(string_value(required(object, "position")));
    rule.next_segment =
        parse_next_segment(string_value(required(object, "next_segment")));

    if (rule.next_segment == WeakFormNextSegment::absent &&
        rule.position == WeakFormPosition::phrase_medial) {
        throw ValidationError(
            "WEAK_FORM_RESOURCE_CONDITION",
            "an absent next segment cannot match a phrase-medial rule");
    }
    if ((rule.next_segment == WeakFormNextSegment::vowel ||
         rule.next_segment == WeakFormNextSegment::non_vowel) &&
        (rule.position == WeakFormPosition::phrase_final ||
         rule.position == WeakFormPosition::single_word_phrase)) {
        throw ValidationError(
            "WEAK_FORM_RESOURCE_CONDITION",
            "a present next segment cannot match a phrase-final rule");
    }
    return rule;
}

WeakFormCapitalization classify_capitalization(std::string_view value) {
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
    if (!saw_letter || all_lower) return WeakFormCapitalization::lower;
    if (all_upper) return WeakFormCapitalization::upper;
    if (title) return WeakFormCapitalization::title;
    return WeakFormCapitalization::mixed;
}

bool matches_position(WeakFormPosition position,
                      std::size_t word_index,
                      std::size_t phrase_start,
                      std::size_t phrase_end) noexcept {
    switch (position) {
        case WeakFormPosition::any: return true;
        case WeakFormPosition::phrase_medial:
            return word_index + 1U < phrase_end;
        case WeakFormPosition::phrase_final:
            return word_index + 1U == phrase_end;
        case WeakFormPosition::single_word_phrase:
            return word_index == phrase_start && word_index + 1U == phrase_end;
    }
    return false;
}

}  // namespace

WeakFormDecision WeakFormRules::decide(
    std::string_view word,
    std::size_t word_index,
    std::size_t phrase_word_start,
    std::size_t phrase_word_end,
    bool has_next_segment,
    std::uint16_t next_segment_id) const {
    WeakFormDecision decision;
    if (word_index < phrase_word_start || word_index >= phrase_word_end ||
        has_next_segment != (word_index + 1U < phrase_word_end)) {
        return decision;
    }
    if (has_next_segment &&
        !std::binary_search(segment_inventory_ids_.begin(),
                            segment_inventory_ids_.end(), next_segment_id)) {
        return decision;
    }
    std::string target;
    if (!ascii_fold_word(word, &target)) {
        return decision;
    }
    const auto found = index_.find(target);
    if (found == index_.end()) {
        return decision;
    }

    decision.kind = WeakFormDecisionKind::no_match;
    const WeakFormCapitalization capitalization =
        classify_capitalization(word);
    const bool next_is_vowel =
        has_next_segment &&
        std::binary_search(vowel_segment_ids_.begin(),
                           vowel_segment_ids_.end(), next_segment_id);
    for (std::size_t rule_index : found->second) {
        const WeakFormRule &rule = rules_[rule_index];
        if (rule.capitalization != WeakFormCapitalization::any &&
            rule.capitalization != capitalization) {
            continue;
        }
        if (!matches_position(rule.position, word_index, phrase_word_start,
                              phrase_word_end)) {
            continue;
        }
        bool next_matches = false;
        switch (rule.next_segment) {
            case WeakFormNextSegment::any: next_matches = true; break;
            case WeakFormNextSegment::vowel:
                next_matches = has_next_segment && next_is_vowel;
                break;
            case WeakFormNextSegment::non_vowel:
                next_matches = has_next_segment && !next_is_vowel;
                break;
            case WeakFormNextSegment::absent:
                next_matches = !has_next_segment;
                break;
        }
        if (!next_matches) {
            continue;
        }
        ++decision.match_count;
        if (decision.match_count == 1U) {
            decision.kind = WeakFormDecisionKind::matched;
            decision.role = rule.role;
            decision.rule_id = rule.rule_id;
        } else {
            decision.kind = WeakFormDecisionKind::ambiguous;
            decision.role.clear();
            decision.rule_id.clear();
        }
    }
    return decision;
}

bool WeakFormRules::contains_target(std::string_view word) const {
    std::string folded;
    return ascii_fold_word(word, &folded) &&
           index_.find(folded) != index_.end();
}

bool WeakFormRules::compatible_with(
    const PronunciationLexicon &lexicon) const {
    return std::all_of(rules_.begin(), rules_.end(),
                       [&](const WeakFormRule &rule) {
                           return lexicon.contains_role(rule.target,
                                                        "default") &&
                                  lexicon.contains_role(rule.target,
                                                        rule.role);
                       });
}

const std::string &WeakFormRules::resource_id() const noexcept {
    return resource_id_;
}

const std::string &WeakFormRules::resource_sha256() const noexcept {
    return resource_sha256_;
}

const std::string &WeakFormRules::base_lexicon_sha256() const noexcept {
    return base_lexicon_sha256_;
}

const std::string &WeakFormRules::segment_inventory_sha256() const noexcept {
    return segment_inventory_sha256_;
}

const std::string &WeakFormRules::review_record_sha256() const noexcept {
    return review_record_sha256_;
}

PronunciationAdmission WeakFormRules::admission() const noexcept {
    return admission_;
}

std::size_t WeakFormRules::rule_count() const noexcept {
    return rules_.size();
}

int load_weak_form_rules(
    std::string_view jsonl,
    std::string_view expected_resource_sha256,
    std::string_view expected_base_lexicon_sha256,
    std::string_view expected_segment_inventory_sha256,
    PronunciationAdmission required_admission,
    const std::vector<SegmentDefinition> &segments,
    WeakFormRules *rules,
    WeakFormResourceFailure *failure) {
    if (rules == nullptr || failure == nullptr) {
        return KGV_INVALID_ARGUMENT;
    }
    *rules = WeakFormRules{};
    *failure = WeakFormResourceFailure{};
    if (!is_lower_sha256(expected_resource_sha256) ||
        !is_lower_sha256(expected_base_lexicon_sha256) ||
        !is_lower_sha256(expected_segment_inventory_sha256) ||
        (required_admission != PronunciationAdmission::product_admitted &&
         required_admission != PronunciationAdmission::test_fixture)) {
        return reject(
            rules, failure, KGV_INVALID_ARGUMENT,
            "INVALID_WEAK_FORM_LOADER_ARGUMENT",
            "weak-form loader requires pinned lowercase hashes and product or test admission",
            0U);
    }

    const std::string actual_inventory_sha256 =
        pronunciation_segment_inventory_sha256(segments);
    if (actual_inventory_sha256.empty() ||
        actual_inventory_sha256 != expected_segment_inventory_sha256) {
        return reject(rules, failure, KGV_ABI_MISMATCH,
                      "WEAK_FORM_RESOURCE_INVENTORY_MISMATCH",
                      "weak-form loader received another segment inventory",
                      0U);
    }
    std::map<std::string, std::uint16_t, std::less<>> segment_ids;
    std::vector<std::uint16_t> inventory_ids;
    inventory_ids.reserve(segments.size());
    for (const SegmentDefinition &segment : segments) {
        segment_ids.emplace(segment.name, segment.id);
        inventory_ids.push_back(segment.id);
    }
    std::sort(inventory_ids.begin(), inventory_ids.end());

    if (jsonl.empty() || jsonl.size() > kMaximumResourceBytes) {
        return reject(rules, failure, KGV_RESOURCE_EXHAUSTED,
                      "WEAK_FORM_RESOURCE_TOO_LARGE",
                      "weak-form resource is empty or exceeds 4 MiB", 0U);
    }
    if (sha256_hex(jsonl) != expected_resource_sha256) {
        return reject(rules, failure, KGV_HASH_MISMATCH,
                      "WEAK_FORM_RESOURCE_HASH_MISMATCH",
                      "weak-form resource does not match its pinned SHA-256",
                      0U);
    }
    if (jsonl.back() != '\n') {
        return reject(rules, failure, KGV_INVALID_MODEL,
                      "WEAK_FORM_RESOURCE_CANONICAL_FORM",
                      "weak-form JSONL must end with LF", 0U);
    }

    WeakFormRules candidate;
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
                          "WEAK_FORM_RESOURCE_CANONICAL_FORM",
                          "weak-form JSONL has an unterminated line",
                          line_number);
        }
        const std::string_view line = jsonl.substr(cursor, end - cursor);
        if (line.empty() || line.size() > kMaximumLineBytes ||
            line.find('\r') != std::string_view::npos) {
            return reject(
                rules, failure, KGV_INVALID_MODEL,
                "WEAK_FORM_RESOURCE_CANONICAL_FORM",
                "weak-form JSONL has an empty, oversized, or CR line",
                line_number);
        }
        json::Value document;
        try {
            document = json::parse(line);
        } catch (const json::ParseError &) {
            return reject(rules, failure, KGV_INVALID_MODEL,
                          "WEAK_FORM_RESOURCE_JSON",
                          "weak-form resource line is not strict JSON",
                          line_number);
        }
        try {
            if (!saw_header) {
                header = parse_header(document, required_admission,
                                      expected_base_lexicon_sha256,
                                      expected_segment_inventory_sha256,
                                      segment_ids);
                saw_header = true;
                candidate.rules_.reserve(header.entry_count);
            } else {
                if (parsed_rules >= header.entry_count) {
                    throw ValidationError("WEAK_FORM_RESOURCE_ENTRY_COUNT",
                                          "weak-form resource has extra rules");
                }
                WeakFormRule rule = parse_rule(document);
                if ((!previous_target.empty() &&
                     rule.target < previous_target) ||
                    (rule.target == previous_target &&
                     rule.rule_id <= previous_rule_id)) {
                    throw ValidationError(
                        "WEAK_FORM_RESOURCE_ORDER",
                        "weak-form rules must be sorted by target and rule ID");
                }
                if (!rule_ids.insert(rule.rule_id).second) {
                    throw ValidationError("WEAK_FORM_DUPLICATE_RULE",
                                          "weak-form rule ID is duplicated");
                }
                previous_target = rule.target;
                previous_rule_id = rule.rule_id;
                const std::size_t rule_index = candidate.rules_.size();
                std::vector<std::size_t> &target_rules =
                    candidate.index_[rule.target];
                if (target_rules.size() >= kMaximumRulesPerTarget) {
                    throw ValidationError(
                        "WEAK_FORM_RESOURCE_LIMIT",
                        "weak-form target exceeds its 64-rule limit");
                }
                target_rules.push_back(rule_index);
                candidate.rules_.push_back(std::move(rule));
                ++parsed_rules;
            }
        } catch (const ValidationError &error) {
            const bool mismatch =
                error.code() == "WEAK_FORM_RESOURCE_LEXICON_MISMATCH" ||
                error.code() == "WEAK_FORM_RESOURCE_INVENTORY_MISMATCH";
            return reject(rules, failure,
                          mismatch ? KGV_ABI_MISMATCH : KGV_INVALID_MODEL,
                          error.code(), error.what(), line_number);
        } catch (const std::exception &) {
            return reject(rules, failure, KGV_INVALID_MODEL,
                          "WEAK_FORM_RESOURCE_SCHEMA",
                          "weak-form resource line has an invalid value type",
                          line_number);
        }
        cursor = end + 1U;
        ++line_number;
    }
    if (!saw_header || parsed_rules != header.entry_count) {
        return reject(rules, failure, KGV_INVALID_MODEL,
                      "WEAK_FORM_RESOURCE_ENTRY_COUNT",
                      "weak-form rule count does not match the header", 1U);
    }

    candidate.resource_id_ = std::move(header.resource_id);
    candidate.resource_sha256_ = std::string(expected_resource_sha256);
    candidate.base_lexicon_sha256_ =
        std::move(header.base_lexicon_sha256);
    candidate.segment_inventory_sha256_ =
        std::move(header.segment_inventory_sha256);
    candidate.review_record_sha256_ =
        std::move(header.review_record_sha256);
    candidate.admission_ = header.admission;
    candidate.vowel_segment_ids_ = std::move(header.vowel_segment_ids);
    candidate.segment_inventory_ids_ = std::move(inventory_ids);
    *rules = std::move(candidate);
    return KGV_OK;
}

const char *weak_form_decision_name(WeakFormDecisionKind value) noexcept {
    switch (value) {
        case WeakFormDecisionKind::not_target: return "NOT_TARGET";
        case WeakFormDecisionKind::no_match: return "NO_MATCH";
        case WeakFormDecisionKind::matched: return "MATCHED";
        case WeakFormDecisionKind::ambiguous: return "AMBIGUOUS";
    }
    return "UNKNOWN";
}

}  // namespace kgv
