#include "frontend/morphology.h"

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

constexpr std::size_t kMaximumResourceBytes = 64U * 1024U;
constexpr std::size_t kMaximumWordBytes = 256U;
constexpr std::size_t kMaximumClassSegments = 64U;
constexpr std::size_t kMaximumSuffixSegments = 4U;
constexpr std::size_t kMaximumSyllables = 16U;
constexpr std::size_t kMaximumSegmentsPerSyllable = 32U;
constexpr std::size_t kMaximumSegmentsPerWord = 128U;

class ValidationError final : public std::runtime_error {
public:
    ValidationError(std::string code, std::string message)
        : std::runtime_error(std::move(message)), code_(std::move(code)) {}

    const std::string &code() const noexcept { return code_; }

private:
    std::string code_;
};

int reject_resource(MorphologyRules *rules,
                    MorphologyResourceFailure *failure,
                    int status,
                    std::string code,
                    std::string message) {
    if (rules != nullptr) {
        *rules = MorphologyRules{};
    }
    if (failure != nullptr) {
        failure->status = status;
        failure->code = std::move(code);
        failure->message = std::move(message);
    }
    return status;
}

int reject_apply(std::vector<PronunciationSyllable> *result,
                 MorphologyApplyFailure *failure,
                 int status,
                 std::string code,
                 std::string message) {
    if (result != nullptr) {
        result->clear();
    }
    if (failure != nullptr) {
        failure->status = status;
        failure->code = std::move(code);
        failure->message = std::move(message);
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
        throw ValidationError("MORPHOLOGY_RESOURCE_SCHEMA",
                              "required morphology field is absent");
    }
    return found->second;
}

const json::Value::Object &object_value(const json::Value &value) {
    if (!value.is_object()) {
        throw ValidationError("MORPHOLOGY_RESOURCE_SCHEMA",
                              "morphology resource must be an object");
    }
    return value.as_object();
}

const json::Value::Array &array_value(const json::Value &value) {
    if (!value.is_array()) {
        throw ValidationError("MORPHOLOGY_RESOURCE_SCHEMA",
                              "morphology segment list must be an array");
    }
    return value.as_array();
}

const std::string &string_value(const json::Value &value) {
    if (!value.is_string()) {
        throw ValidationError("MORPHOLOGY_RESOURCE_SCHEMA",
                              "morphology value must be a string");
    }
    return value.as_string();
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

PronunciationAdmission parse_admission(std::string_view value) {
    if (value == "product-admitted") {
        return PronunciationAdmission::product_admitted;
    }
    if (value == "test-fixture") {
        return PronunciationAdmission::test_fixture;
    }
    throw ValidationError("MORPHOLOGY_RESOURCE_ADMISSION",
                          "morphology admission is unknown or unsupported");
}

std::vector<std::uint16_t> parse_segments(
    const json::Value &value,
    const std::map<std::string, std::uint16_t, std::less<>> &segment_ids,
    std::size_t maximum,
    bool require_sorted,
    std::string_view description) {
    const json::Value::Array &values = array_value(value);
    if (values.empty() || values.size() > maximum) {
        throw ValidationError("MORPHOLOGY_RESOURCE_LIMIT",
                              std::string(description) +
                                  " count is outside its limit");
    }
    std::vector<std::uint16_t> parsed;
    parsed.reserve(values.size());
    std::set<std::uint16_t> unique;
    for (const json::Value &item : values) {
        const std::string &name = string_value(item);
        const auto found = segment_ids.find(name);
        if (found == segment_ids.end()) {
            throw ValidationError("MORPHOLOGY_UNKNOWN_SEGMENT",
                                  std::string(description) +
                                      " uses a segment outside the inventory");
        }
        if (!unique.insert(found->second).second) {
            throw ValidationError("MORPHOLOGY_RESOURCE_SCHEMA",
                                  std::string(description) +
                                      " contains a duplicate segment");
        }
        if (require_sorted && !parsed.empty() &&
            parsed.back() >= found->second) {
            throw ValidationError("MORPHOLOGY_RESOURCE_ORDER",
                                  std::string(description) +
                                      " must follow increasing segment IDs");
        }
        parsed.push_back(found->second);
    }
    return parsed;
}

bool overlaps(const std::vector<std::uint16_t> &left,
              const std::vector<std::uint16_t> &right) {
    return std::any_of(left.begin(), left.end(), [&](std::uint16_t value) {
        return std::binary_search(right.begin(), right.end(), value);
    });
}

bool ascii_letter(char value) noexcept {
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z');
}

char ascii_fold(char value) noexcept {
    return value >= 'A' && value <= 'Z'
               ? static_cast<char>(value + ('a' - 'A'))
               : value;
}

bool ascii_word(std::string_view value) noexcept {
    return !value.empty() && value.size() <= kMaximumWordBytes &&
           std::all_of(value.begin(), value.end(), ascii_letter);
}

bool ends_with_folded(std::string_view value,
                      std::string_view suffix) noexcept {
    if (value.size() < suffix.size()) {
        return false;
    }
    const std::size_t start = value.size() - suffix.size();
    for (std::size_t index = 0U; index < suffix.size(); ++index) {
        if (ascii_fold(value[start + index]) != suffix[index]) {
            return false;
        }
    }
    return true;
}

std::string folded(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (char character : value) {
        result.push_back(ascii_fold(character));
    }
    return result;
}

char replacement_y(std::string_view word) noexcept {
    return word.size() >= 3U && word[word.size() - 3U] >= 'A' &&
                   word[word.size() - 3U] <= 'Z'
               ? 'Y'
               : 'y';
}

void add_candidate(std::vector<MorphologyCandidate> *candidates,
                   MorphologyKind kind,
                   std::string stem) {
    if (stem.size() < 2U || !ascii_word(stem)) {
        return;
    }
    const std::string key = folded(stem);
    const bool duplicate = std::any_of(
        candidates->begin(), candidates->end(),
        [&](const MorphologyCandidate &candidate) {
            return candidate.kind == kind && folded(candidate.stem) == key;
        });
    if (!duplicate) {
        candidates->push_back(MorphologyCandidate{kind, std::move(stem)});
    }
}

bool valid_stem_syllables(
    const std::vector<PronunciationSyllable> &syllables,
    const std::vector<std::uint16_t> &segment_inventory_ids,
    std::size_t *segment_count) {
    if (syllables.empty() || syllables.size() > kMaximumSyllables ||
        segment_inventory_ids.empty()) {
        return false;
    }
    *segment_count = 0U;
    std::size_t primary_count = 0U;
    for (const PronunciationSyllable &syllable : syllables) {
        if (syllable.segment_ids.empty() ||
            syllable.segment_ids.size() > kMaximumSegmentsPerSyllable ||
            (syllable.stress != SyllableStress::none &&
             syllable.stress != SyllableStress::primary &&
             syllable.stress != SyllableStress::secondary)) {
            return false;
        }
        if (!std::all_of(
                syllable.segment_ids.begin(), syllable.segment_ids.end(),
                [&](std::uint16_t segment_id) {
                    return std::binary_search(segment_inventory_ids.begin(),
                                              segment_inventory_ids.end(),
                                              segment_id);
                })) {
            return false;
        }
        if (syllable.stress == SyllableStress::primary &&
            ++primary_count > 1U) {
            return false;
        }
        *segment_count += syllable.segment_ids.size();
        if (*segment_count > kMaximumSegmentsPerWord) {
            return false;
        }
    }
    return true;
}

}  // namespace

int MorphologyRules::apply(
    MorphologyKind kind,
    const std::vector<PronunciationSyllable> &stem,
    std::vector<PronunciationSyllable> *result,
    MorphologyApplyFailure *failure) const {
    if (result == nullptr || failure == nullptr) {
        return KGV_INVALID_ARGUMENT;
    }
    result->clear();
    *failure = MorphologyApplyFailure{};
    if (resource_sha256_.empty()) {
        return reject_apply(result, failure, KGV_INVALID_STATE,
                            "MORPHOLOGY_RULES_NOT_LOADED",
                            "morphology rules are not loaded");
    }
    std::size_t segment_count = 0U;
    if (!valid_stem_syllables(stem, segment_inventory_ids_,
                              &segment_count)) {
        return reject_apply(result, failure, KGV_INVALID_ARGUMENT,
                            "INVALID_MORPHOLOGY_STEM",
                            "morphology requires a bounded resolved stem");
    }
    const std::uint16_t final_segment = stem.back().segment_ids.back();
    const std::vector<std::uint16_t> *suffix = nullptr;
    bool new_syllable = false;
    switch (kind) {
        case MorphologyKind::plural_or_possessive:
            if (std::binary_search(plural_sibilant_finals_.begin(),
                                   plural_sibilant_finals_.end(),
                                   final_segment)) {
                suffix = &plural_sibilant_suffix_;
                new_syllable = true;
            } else if (std::binary_search(plural_unvoiced_finals_.begin(),
                                          plural_unvoiced_finals_.end(),
                                          final_segment)) {
                suffix = &plural_unvoiced_suffix_;
            } else {
                suffix = &plural_voiced_suffix_;
            }
            break;
        case MorphologyKind::past:
            if (std::binary_search(past_syllabic_finals_.begin(),
                                   past_syllabic_finals_.end(),
                                   final_segment)) {
                suffix = &past_syllabic_suffix_;
                new_syllable = true;
            } else if (std::binary_search(past_unvoiced_finals_.begin(),
                                          past_unvoiced_finals_.end(),
                                          final_segment)) {
                suffix = &past_unvoiced_suffix_;
            } else {
                suffix = &past_voiced_suffix_;
            }
            break;
        case MorphologyKind::progressive:
            suffix = &progressive_suffix_;
            new_syllable = true;
            break;
        default:
            return reject_apply(result, failure, KGV_INVALID_ARGUMENT,
                                "UNKNOWN_MORPHOLOGY_KIND",
                                "morphology kind is unsupported");
    }
    if (suffix == nullptr || suffix->empty() ||
        segment_count + suffix->size() > kMaximumSegmentsPerWord ||
        (new_syllable && stem.size() >= kMaximumSyllables) ||
        (!new_syllable && stem.back().segment_ids.size() + suffix->size() >
                              kMaximumSegmentsPerSyllable)) {
        return reject_apply(result, failure, KGV_INVALID_TEXT,
                            "MORPHOLOGY_RESULT_TOO_LARGE",
                            "inflected pronunciation exceeds word limits");
    }
    *result = stem;
    if (new_syllable) {
        result->push_back(PronunciationSyllable{
            SyllableStress::none,
            *suffix,
        });
    } else {
        result->back().segment_ids.insert(result->back().segment_ids.end(),
                                          suffix->begin(), suffix->end());
    }
    return KGV_OK;
}

const std::string &MorphologyRules::resource_id() const noexcept {
    return resource_id_;
}

const std::string &MorphologyRules::resource_sha256() const noexcept {
    return resource_sha256_;
}

const std::string &MorphologyRules::base_lexicon_sha256() const noexcept {
    return base_lexicon_sha256_;
}

const std::string &MorphologyRules::segment_inventory_sha256() const noexcept {
    return segment_inventory_sha256_;
}

const std::string &MorphologyRules::review_record_sha256() const noexcept {
    return review_record_sha256_;
}

PronunciationAdmission MorphologyRules::admission() const noexcept {
    return admission_;
}

int load_morphology_rules(
    std::string_view document_bytes,
    std::string_view expected_resource_sha256,
    std::string_view expected_base_lexicon_sha256,
    std::string_view expected_segment_inventory_sha256,
    PronunciationAdmission required_admission,
    const std::vector<SegmentDefinition> &segments,
    MorphologyRules *rules,
    MorphologyResourceFailure *failure) {
    if (rules == nullptr || failure == nullptr) {
        return KGV_INVALID_ARGUMENT;
    }
    *rules = MorphologyRules{};
    *failure = MorphologyResourceFailure{};
    if (!is_lower_sha256(expected_resource_sha256) ||
        !is_lower_sha256(expected_base_lexicon_sha256) ||
        !is_lower_sha256(expected_segment_inventory_sha256) ||
        segments.empty() ||
        (required_admission != PronunciationAdmission::product_admitted &&
         required_admission != PronunciationAdmission::test_fixture)) {
        return reject_resource(rules, failure, KGV_INVALID_ARGUMENT,
                               "INVALID_MORPHOLOGY_LOADER_ARGUMENT",
                               "morphology loader requires pinned hashes, segments, and product or test admission");
    }
    if (pronunciation_segment_inventory_sha256(segments) !=
        expected_segment_inventory_sha256) {
        return reject_resource(rules, failure, KGV_ABI_MISMATCH,
                               "MORPHOLOGY_SEGMENT_INVENTORY_HASH_MISMATCH",
                               "segment definitions do not match the pinned inventory");
    }
    if (document_bytes.empty() ||
        document_bytes.size() > kMaximumResourceBytes) {
        return reject_resource(rules, failure, KGV_RESOURCE_EXHAUSTED,
                               "MORPHOLOGY_RESOURCE_TOO_LARGE",
                               "morphology resource is empty or exceeds 64 KiB");
    }
    if (sha256_hex(document_bytes) != expected_resource_sha256) {
        return reject_resource(rules, failure, KGV_HASH_MISMATCH,
                               "MORPHOLOGY_RESOURCE_HASH_MISMATCH",
                               "morphology resource does not match its pinned SHA-256");
    }
    if (document_bytes.back() != '\n' ||
        document_bytes.substr(0U, document_bytes.size() - 1U).find('\n') !=
            std::string_view::npos ||
        document_bytes.find('\r') != std::string_view::npos) {
        return reject_resource(rules, failure, KGV_INVALID_MODEL,
                               "MORPHOLOGY_RESOURCE_CANONICAL_FORM",
                               "morphology resource must be one LF-terminated JSON line");
    }

    json::Value document;
    try {
        document = json::parse(
            document_bytes.substr(0U, document_bytes.size() - 1U));
    } catch (const json::ParseError &) {
        return reject_resource(rules, failure, KGV_INVALID_MODEL,
                               "MORPHOLOGY_RESOURCE_JSON",
                               "morphology resource is not strict JSON");
    }

    try {
        const json::Value::Object &object = object_value(document);
        if (!exact_keys(object,
                        {"admission", "base_lexicon_sha256", "dialect",
                         "past_syllabic_finals", "past_syllabic_suffix",
                         "past_unvoiced_finals", "past_unvoiced_suffix",
                         "past_voiced_suffix", "plural_sibilant_finals",
                         "plural_sibilant_suffix",
                         "plural_unvoiced_finals",
                         "plural_unvoiced_suffix", "plural_voiced_suffix",
                         "progressive_suffix", "resource_id",
                         "review_record_sha256", "schema",
                         "segment_inventory_sha256"})) {
            throw ValidationError(
                "MORPHOLOGY_RESOURCE_SCHEMA",
                "morphology resource has unknown or absent fields");
        }
        if (string_value(required(object, "schema")) !=
            "kilix.voicegen.morphology-rules/v1") {
            throw ValidationError("MORPHOLOGY_RESOURCE_SCHEMA",
                                  "morphology schema is unsupported");
        }
        if (string_value(required(object, "dialect")) != "en-AU") {
            throw ValidationError("MORPHOLOGY_RESOURCE_DIALECT",
                                  "morphology dialect must be en-AU");
        }
        MorphologyRules candidate;
        candidate.admission_ =
            parse_admission(string_value(required(object, "admission")));
        if (candidate.admission_ != required_admission) {
            throw ValidationError("MORPHOLOGY_RESOURCE_ADMISSION",
                                  "morphology admission does not match the caller");
        }
        candidate.resource_id_ =
            string_value(required(object, "resource_id"));
        if (!stable_identifier(candidate.resource_id_, 128U)) {
            throw ValidationError("MORPHOLOGY_RESOURCE_SCHEMA",
                                  "morphology resource ID is not canonical");
        }
        candidate.base_lexicon_sha256_ =
            string_value(required(object, "base_lexicon_sha256"));
        candidate.segment_inventory_sha256_ =
            string_value(required(object, "segment_inventory_sha256"));
        if (!is_lower_sha256(candidate.base_lexicon_sha256_) ||
            !is_lower_sha256(candidate.segment_inventory_sha256_)) {
            throw ValidationError("MORPHOLOGY_RESOURCE_SCHEMA",
                                  "morphology binding is not lowercase SHA-256");
        }
        if (candidate.base_lexicon_sha256_ !=
            expected_base_lexicon_sha256) {
            throw ValidationError("MORPHOLOGY_RESOURCE_LEXICON_MISMATCH",
                                  "morphology rules target another base lexicon");
        }
        if (candidate.segment_inventory_sha256_ !=
            expected_segment_inventory_sha256) {
            throw ValidationError("MORPHOLOGY_RESOURCE_INVENTORY_MISMATCH",
                                  "morphology rules target another segment inventory");
        }
        const json::Value &review =
            required(object, "review_record_sha256");
        if (candidate.admission_ ==
            PronunciationAdmission::product_admitted) {
            if (!review.is_string()) {
                throw ValidationError("MORPHOLOGY_RESOURCE_ADMISSION",
                                      "product morphology lacks a review hash");
            }
            candidate.review_record_sha256_ = string_value(review);
            if (!is_lower_sha256(candidate.review_record_sha256_)) {
                throw ValidationError("MORPHOLOGY_RESOURCE_ADMISSION",
                                      "product morphology review hash is invalid");
            }
        } else if (!review.is_null()) {
            throw ValidationError("MORPHOLOGY_RESOURCE_ADMISSION",
                                  "test morphology must not claim product review");
        }

        std::map<std::string, std::uint16_t, std::less<>> segment_ids;
        for (const SegmentDefinition &segment : segments) {
            segment_ids.emplace(segment.name, segment.id);
            candidate.segment_inventory_ids_.push_back(segment.id);
        }
        candidate.plural_sibilant_finals_ = parse_segments(
            required(object, "plural_sibilant_finals"), segment_ids,
            kMaximumClassSegments, true, "plural sibilant class");
        candidate.plural_unvoiced_finals_ = parse_segments(
            required(object, "plural_unvoiced_finals"), segment_ids,
            kMaximumClassSegments, true, "plural unvoiced class");
        candidate.plural_sibilant_suffix_ = parse_segments(
            required(object, "plural_sibilant_suffix"), segment_ids,
            kMaximumSuffixSegments, false, "plural sibilant suffix");
        candidate.plural_unvoiced_suffix_ = parse_segments(
            required(object, "plural_unvoiced_suffix"), segment_ids,
            kMaximumSuffixSegments, false, "plural unvoiced suffix");
        candidate.plural_voiced_suffix_ = parse_segments(
            required(object, "plural_voiced_suffix"), segment_ids,
            kMaximumSuffixSegments, false, "plural voiced suffix");
        candidate.past_syllabic_finals_ = parse_segments(
            required(object, "past_syllabic_finals"), segment_ids,
            kMaximumClassSegments, true, "past syllabic class");
        candidate.past_unvoiced_finals_ = parse_segments(
            required(object, "past_unvoiced_finals"), segment_ids,
            kMaximumClassSegments, true, "past unvoiced class");
        candidate.past_syllabic_suffix_ = parse_segments(
            required(object, "past_syllabic_suffix"), segment_ids,
            kMaximumSuffixSegments, false, "past syllabic suffix");
        candidate.past_unvoiced_suffix_ = parse_segments(
            required(object, "past_unvoiced_suffix"), segment_ids,
            kMaximumSuffixSegments, false, "past unvoiced suffix");
        candidate.past_voiced_suffix_ = parse_segments(
            required(object, "past_voiced_suffix"), segment_ids,
            kMaximumSuffixSegments, false, "past voiced suffix");
        candidate.progressive_suffix_ = parse_segments(
            required(object, "progressive_suffix"), segment_ids,
            kMaximumSuffixSegments, false, "progressive suffix");
        if (overlaps(candidate.plural_sibilant_finals_,
                     candidate.plural_unvoiced_finals_) ||
            overlaps(candidate.past_syllabic_finals_,
                     candidate.past_unvoiced_finals_)) {
            throw ValidationError("MORPHOLOGY_RESOURCE_CLASS_OVERLAP",
                                  "morphology final-segment classes overlap");
        }
        candidate.resource_sha256_ =
            std::string(expected_resource_sha256);
        *rules = std::move(candidate);
        return KGV_OK;
    } catch (const ValidationError &error) {
        const int status =
            error.code() == "MORPHOLOGY_RESOURCE_LEXICON_MISMATCH" ||
                    error.code() == "MORPHOLOGY_RESOURCE_INVENTORY_MISMATCH"
                ? KGV_ABI_MISMATCH
                : KGV_INVALID_MODEL;
        return reject_resource(rules, failure, status, error.code(),
                               error.what());
    } catch (const std::exception &) {
        return reject_resource(rules, failure, KGV_INVALID_MODEL,
                               "MORPHOLOGY_RESOURCE_SCHEMA",
                               "morphology resource has an invalid value type");
    }
}

std::vector<MorphologyCandidate> morphology_candidates(
    std::string_view word) {
    std::vector<MorphologyCandidate> candidates;
    if (word.size() < 3U || word.size() > kMaximumWordBytes) {
        return candidates;
    }

    const bool ascii_apostrophe_s =
        word.size() > 2U && word[word.size() - 2U] == '\'' &&
        ascii_fold(word.back()) == 's';
    constexpr std::string_view curly_apostrophe = "\xe2\x80\x99";
    const bool curly_apostrophe_s =
        word.size() > curly_apostrophe.size() + 1U &&
        ascii_fold(word.back()) == 's' &&
        word.substr(word.size() - curly_apostrophe.size() - 1U,
                    curly_apostrophe.size()) == curly_apostrophe;
    if (ascii_apostrophe_s) {
        add_candidate(&candidates, MorphologyKind::plural_or_possessive,
                      std::string(word.substr(0U, word.size() - 2U)));
    } else if (curly_apostrophe_s) {
        add_candidate(
            &candidates, MorphologyKind::plural_or_possessive,
            std::string(word.substr(
                0U, word.size() - curly_apostrophe.size() - 1U)));
    } else if (ascii_word(word) && ends_with_folded(word, "s") &&
               !ends_with_folded(word, "ss")) {
        if (ends_with_folded(word, "ies") && word.size() > 4U) {
            std::string stem(word.substr(0U, word.size() - 3U));
            stem.push_back(replacement_y(word));
            add_candidate(&candidates,
                          MorphologyKind::plural_or_possessive,
                          std::move(stem));
        }
        if (ends_with_folded(word, "es") && word.size() > 3U) {
            add_candidate(&candidates,
                          MorphologyKind::plural_or_possessive,
                          std::string(word.substr(0U, word.size() - 2U)));
        }
        add_candidate(&candidates, MorphologyKind::plural_or_possessive,
                      std::string(word.substr(0U, word.size() - 1U)));
    }

    if (ascii_word(word) && word.size() >= 4U &&
        ends_with_folded(word, "ed")) {
        if (!ends_with_folded(word, "eed")) {
            add_candidate(&candidates, MorphologyKind::past,
                          std::string(word.substr(0U, word.size() - 2U)));
        }
        add_candidate(&candidates, MorphologyKind::past,
                      std::string(word.substr(0U, word.size() - 1U)));
        if (ends_with_folded(word, "ied") && word.size() > 4U) {
            std::string stem(word.substr(0U, word.size() - 3U));
            stem.push_back(replacement_y(word));
            add_candidate(&candidates, MorphologyKind::past,
                          std::move(stem));
        }
        if (word.size() > 5U &&
            ascii_fold(word[word.size() - 3U]) ==
                ascii_fold(word[word.size() - 4U])) {
            add_candidate(&candidates, MorphologyKind::past,
                          std::string(word.substr(0U, word.size() - 3U)));
        }
        if (word.size() > 5U &&
            ascii_fold(word[word.size() - 3U]) == 'k' &&
            ascii_fold(word[word.size() - 4U]) == 'c') {
            add_candidate(&candidates, MorphologyKind::past,
                          std::string(word.substr(0U, word.size() - 3U)));
        }
    }

    if (ascii_word(word) && word.size() >= 5U &&
        ends_with_folded(word, "ing")) {
        const std::string base(word.substr(0U, word.size() - 3U));
        add_candidate(&candidates, MorphologyKind::progressive, base);
        add_candidate(&candidates, MorphologyKind::progressive, base + "e");
        if (word.size() > 6U &&
            ascii_fold(word[word.size() - 4U]) ==
                ascii_fold(word[word.size() - 5U])) {
            add_candidate(&candidates, MorphologyKind::progressive,
                          std::string(word.substr(0U, word.size() - 4U)));
        }
        if (word.size() > 6U &&
            ascii_fold(word[word.size() - 4U]) == 'k' &&
            ascii_fold(word[word.size() - 5U]) == 'c') {
            add_candidate(&candidates, MorphologyKind::progressive,
                          std::string(word.substr(0U, word.size() - 4U)));
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const MorphologyCandidate &left,
                 const MorphologyCandidate &right) {
                  if (left.kind != right.kind) {
                      return static_cast<int>(left.kind) <
                             static_cast<int>(right.kind);
                  }
                  const std::string left_folded = folded(left.stem);
                  const std::string right_folded = folded(right.stem);
                  return left_folded < right_folded ||
                         (left_folded == right_folded &&
                          left.stem < right.stem);
              });
    return candidates;
}

const char *morphology_kind_name(MorphologyKind value) noexcept {
    switch (value) {
        case MorphologyKind::plural_or_possessive:
            return "PLURAL_OR_POSSESSIVE";
        case MorphologyKind::past: return "PAST";
        case MorphologyKind::progressive: return "PROGRESSIVE";
    }
    return "UNKNOWN";
}

}  // namespace kgv
