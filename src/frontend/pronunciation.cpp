#include "frontend/pronunciation.h"

#include <algorithm>
#include <array>
#include <charconv>
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

#include <utf8proc.h>

#include "frontend/frontend.h"
#include "runtime/json.h"
#include "runtime/sha256.h"

namespace kgv {
namespace {

constexpr std::size_t kMaximumResourceBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaximumLineBytes = 64U * 1024U;
constexpr std::size_t kMaximumEntries = 200000U;
constexpr std::size_t kMaximumGraphemeBytes = 256U;
constexpr std::size_t kMaximumRoles = 16U;
constexpr std::size_t kMaximumSyllables = 16U;
constexpr std::size_t kMaximumSegmentsPerSyllable = 32U;
constexpr std::size_t kMaximumSegmentsPerEntry = 128U;

class ValidationError final : public std::runtime_error {
public:
    ValidationError(std::string code, std::string message)
        : std::runtime_error(std::move(message)), code_(std::move(code)) {}

    const std::string &code() const noexcept { return code_; }

private:
    std::string code_;
};

int reject(PronunciationLexicon *lexicon,
           PronunciationResourceFailure *failure,
           int status,
           std::string code,
           std::string message,
           std::size_t line) {
    if (lexicon != nullptr) {
        *lexicon = PronunciationLexicon{};
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
        throw ValidationError("PRONUNCIATION_RESOURCE_SCHEMA",
                              "required pronunciation resource field is absent");
    }
    return found->second;
}

const json::Value::Object &object_value(const json::Value &value) {
    if (!value.is_object()) {
        throw ValidationError("PRONUNCIATION_RESOURCE_SCHEMA",
                              "pronunciation resource value must be an object");
    }
    return value.as_object();
}

const json::Value::Array &array_value(const json::Value &value) {
    if (!value.is_array()) {
        throw ValidationError("PRONUNCIATION_RESOURCE_SCHEMA",
                              "pronunciation resource value must be an array");
    }
    return value.as_array();
}

const std::string &string_value(const json::Value &value) {
    if (!value.is_string()) {
        throw ValidationError("PRONUNCIATION_RESOURCE_SCHEMA",
                              "pronunciation resource value must be a string");
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
        throw ValidationError("PRONUNCIATION_RESOURCE_SCHEMA",
                              std::string(description) + " must be an integer");
    }
    if (number < 0 || static_cast<std::uint64_t>(number) > maximum) {
        throw ValidationError("PRONUNCIATION_RESOURCE_LIMIT",
                              std::string(description) + " exceeds its limit");
    }
    return static_cast<std::size_t>(number);
}

bool stable_identifier(std::string_view value, std::size_t maximum) noexcept {
    if (value.empty() || value.size() > maximum ||
        value.front() < 'a' || value.front() > 'z') {
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
    if (value.empty() || value.size() > 32U ||
        value.front() < 'a' || value.front() > 'z') {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '-';
    });
}

bool stable_segment_name(std::string_view value) noexcept {
    if (value.empty() || value.size() > 32U ||
        value.front() < 'A' || value.front() > 'Z') {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '_';
    });
}

std::string canonical_segment_inventory(
    const std::vector<SegmentDefinition> &segments) {
    if (segments.empty()) {
        return {};
    }
    std::set<std::string, std::less<>> names;
    std::uint16_t previous_id = 0U;
    std::string canonical;
    for (const SegmentDefinition &segment : segments) {
        if (!stable_segment_name(segment.name) || segment.id == 0U ||
            segment.id <= previous_id || !names.insert(segment.name).second) {
            return {};
        }
        std::array<char, 5> digits{};
        const auto encoded = std::to_chars(digits.data(),
                                           digits.data() + digits.size(),
                                           segment.id);
        if (encoded.ec != std::errc{}) {
            return {};
        }
        canonical.append(digits.data(), encoded.ptr);
        canonical.push_back('\t');
        canonical.append(segment.name);
        canonical.push_back('\n');
        previous_id = segment.id;
    }
    return canonical;
}

void append_utf8(std::uint32_t scalar, std::string *output) {
    if (scalar <= 0x7fU) {
        output->push_back(static_cast<char>(scalar));
    } else if (scalar <= 0x7ffU) {
        output->push_back(static_cast<char>(0xc0U | (scalar >> 6U)));
        output->push_back(static_cast<char>(0x80U | (scalar & 0x3fU)));
    } else if (scalar <= 0xffffU) {
        output->push_back(static_cast<char>(0xe0U | (scalar >> 12U)));
        output->push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3fU)));
        output->push_back(static_cast<char>(0x80U | (scalar & 0x3fU)));
    } else {
        output->push_back(static_cast<char>(0xf0U | (scalar >> 18U)));
        output->push_back(static_cast<char>(0x80U | ((scalar >> 12U) & 0x3fU)));
        output->push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3fU)));
        output->push_back(static_cast<char>(0x80U | (scalar & 0x3fU)));
    }
}

bool letter_scalar(std::uint32_t scalar) noexcept {
    const utf8proc_category_t category =
        utf8proc_category(static_cast<utf8proc_int32_t>(scalar));
    return category == UTF8PROC_CATEGORY_LU || category == UTF8PROC_CATEGORY_LL ||
           category == UTF8PROC_CATEGORY_LT || category == UTF8PROC_CATEGORY_LM ||
           category == UTF8PROC_CATEGORY_LO;
}

bool mark_scalar(std::uint32_t scalar) noexcept {
    const utf8proc_category_t category =
        utf8proc_category(static_cast<utf8proc_int32_t>(scalar));
    return category == UTF8PROC_CATEGORY_MN || category == UTF8PROC_CATEGORY_MC;
}

bool grapheme_scalar(std::uint32_t scalar) noexcept {
    return letter_scalar(scalar) || mark_scalar(scalar) || scalar == 0x0027U ||
           scalar == 0x2019U || scalar == 0x002dU;
}

void validate_grapheme(std::string_view grapheme) {
    if (grapheme.empty() || grapheme.size() > kMaximumGraphemeBytes) {
        throw ValidationError("PRONUNCIATION_INVALID_GRAPHEME",
                              "grapheme is empty or exceeds 256 bytes");
    }
    FrontendAnalysis analysis;
    FrontendFailure failure;
    if (analyze_frontend(grapheme, KGV_PROFILE_PROSE, &analysis, &failure) != KGV_OK ||
        analysis.visible_scalars.empty()) {
        throw ValidationError("PRONUNCIATION_INVALID_GRAPHEME",
                              "grapheme is not accepted strict UTF-8 text");
    }
    for (std::size_t index = 0U; index < analysis.visible_scalars.size(); ++index) {
        const std::uint32_t scalar = analysis.visible_scalars[index].value;
        if (!grapheme_scalar(scalar)) {
            throw ValidationError("PRONUNCIATION_INVALID_GRAPHEME",
                                  "grapheme contains a non-lexical scalar");
        }
        const bool punctuation = scalar == 0x0027U || scalar == 0x2019U ||
                                 scalar == 0x002dU;
        if (index == 0U && !letter_scalar(scalar)) {
            throw ValidationError("PRONUNCIATION_INVALID_GRAPHEME",
                                  "grapheme must begin with a letter");
        }
        if (punctuation &&
            (index == 0U || index + 1U == analysis.visible_scalars.size())) {
            throw ValidationError("PRONUNCIATION_INVALID_GRAPHEME",
                                  "grapheme punctuation must be internal");
        }
        if (punctuation && index > 0U) {
            const std::uint32_t previous = analysis.visible_scalars[index - 1U].value;
            const std::uint32_t next = analysis.visible_scalars[index + 1U].value;
            if (!letter_scalar(previous) || !letter_scalar(next)) {
                throw ValidationError("PRONUNCIATION_INVALID_GRAPHEME",
                                      "grapheme punctuation must separate letters");
            }
        }
    }
    std::vector<FrontendScalar> normalized;
    if (normalize_frontend_nfc(grapheme, analysis.visible_scalars,
                               &normalized, &failure) != KGV_OK) {
        throw ValidationError("PRONUNCIATION_INVALID_GRAPHEME",
                              "grapheme could not be normalized");
    }
    std::string canonical;
    for (const FrontendScalar &scalar : normalized) {
        append_utf8(scalar.value, &canonical);
    }
    if (canonical != grapheme) {
        throw ValidationError("PRONUNCIATION_GRAPHEME_NOT_NFC",
                              "grapheme must already be NFC-normalized");
    }
}

bool ascii_fold(std::string_view value, std::string *folded) {
    if (value.empty()) {
        return false;
    }
    folded->clear();
    folded->reserve(value.size());
    for (std::size_t index = 0U; index < value.size(); ++index) {
        char character = value[index];
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character + ('a' - 'A'));
        } else if (!((character >= 'a' && character <= 'z') ||
                     character == '\'' || character == '-')) {
            return false;
        }
        if ((character == '\'' || character == '-') &&
            (index == 0U || index + 1U == value.size())) {
            return false;
        }
        folded->push_back(character);
    }
    return true;
}

std::string index_key(char case_code,
                      std::string_view grapheme,
                      std::string_view role) {
    std::string key;
    key.reserve(grapheme.size() + role.size() + 3U);
    key.push_back(case_code);
    key.push_back('\0');
    key.append(grapheme);
    key.push_back('\0');
    key.append(role);
    return key;
}

PronunciationAdmission parse_admission(std::string_view value) {
    if (value == "product-admitted") {
        return PronunciationAdmission::product_admitted;
    }
    if (value == "local-user") {
        return PronunciationAdmission::local_user;
    }
    if (value == "test-fixture") {
        return PronunciationAdmission::test_fixture;
    }
    throw ValidationError("PRONUNCIATION_RESOURCE_ADMISSION",
                          "pronunciation resource admission is unknown");
}

SyllableStress parse_stress(std::string_view value) {
    if (value == "none") return SyllableStress::none;
    if (value == "primary") return SyllableStress::primary;
    if (value == "secondary") return SyllableStress::secondary;
    throw ValidationError("PRONUNCIATION_RESOURCE_SCHEMA",
                          "syllable stress is unknown");
}

struct Header final {
    std::string resource_id;
    std::string inventory_sha256;
    std::string review_sha256;
    PronunciationAdmission admission = PronunciationAdmission::test_fixture;
    std::size_t entry_count = 0U;
};

Header parse_header(const json::Value &document,
                    PronunciationAdmission required_admission,
                    std::string_view expected_inventory_sha256) {
    const json::Value::Object &object = object_value(document);
    if (!exact_keys(object, {"admission", "dialect", "entry_count", "resource_id",
                             "review_record_sha256", "schema",
                             "segment_inventory_sha256"})) {
        throw ValidationError("PRONUNCIATION_RESOURCE_SCHEMA",
                              "pronunciation resource header has unknown or absent fields");
    }
    if (string_value(required(object, "schema")) !=
        "kilix.voicegen.pronunciation-lexicon/v1") {
        throw ValidationError("PRONUNCIATION_RESOURCE_SCHEMA",
                              "pronunciation resource header schema is unsupported");
    }
    if (string_value(required(object, "dialect")) != "en-AU") {
        throw ValidationError("PRONUNCIATION_RESOURCE_DIALECT",
                              "pronunciation resource dialect must be en-AU");
    }
    Header header;
    header.admission = parse_admission(string_value(required(object, "admission")));
    if (header.admission != required_admission) {
        throw ValidationError("PRONUNCIATION_RESOURCE_ADMISSION",
                              "pronunciation resource admission does not match the caller");
    }
    header.resource_id = string_value(required(object, "resource_id"));
    if (!stable_identifier(header.resource_id, 128U)) {
        throw ValidationError("PRONUNCIATION_RESOURCE_SCHEMA",
                              "pronunciation resource ID is not canonical");
    }
    header.inventory_sha256 =
        string_value(required(object, "segment_inventory_sha256"));
    if (!is_lower_sha256(header.inventory_sha256)) {
        throw ValidationError("PRONUNCIATION_RESOURCE_SCHEMA",
                              "segment inventory hash is not lowercase SHA-256");
    }
    if (header.inventory_sha256 != expected_inventory_sha256) {
        throw ValidationError("PRONUNCIATION_RESOURCE_INVENTORY_MISMATCH",
                              "pronunciation resource targets another segment inventory");
    }
    header.entry_count =
        bounded_count(required(object, "entry_count"), kMaximumEntries,
                      "pronunciation entry count");
    if (header.admission == PronunciationAdmission::product_admitted &&
        header.entry_count == 0U) {
        throw ValidationError("PRONUNCIATION_RESOURCE_ADMISSION",
                              "a product-admitted lexicon cannot be empty");
    }
    const json::Value &review = required(object, "review_record_sha256");
    if (header.admission == PronunciationAdmission::product_admitted) {
        if (!review.is_string()) {
            throw ValidationError("PRONUNCIATION_RESOURCE_ADMISSION",
                                  "product lexicon lacks a review record hash");
        }
        header.review_sha256 = string_value(review);
        if (!is_lower_sha256(header.review_sha256)) {
            throw ValidationError("PRONUNCIATION_RESOURCE_ADMISSION",
                                  "product lexicon lacks a valid review record hash");
        }
    } else if (!review.is_null()) {
        throw ValidationError("PRONUNCIATION_RESOURCE_ADMISSION",
                              "non-product lexicon must not claim a review record");
    }
    return header;
}

PronunciationEntry parse_entry(
    const json::Value &document,
    const std::map<std::string, std::uint16_t, std::less<>> &segment_ids) {
    const json::Value::Object &object = object_value(document);
    if (!exact_keys(object, {"case", "grapheme", "roles", "schema", "source",
                             "syllables"})) {
        throw ValidationError("PRONUNCIATION_RESOURCE_SCHEMA",
                              "pronunciation entry has unknown or absent fields");
    }
    if (string_value(required(object, "schema")) !=
        "kilix.voicegen.pronunciation-entry/v1") {
        throw ValidationError("PRONUNCIATION_RESOURCE_SCHEMA",
                              "pronunciation entry schema is unsupported");
    }

    PronunciationEntry entry;
    entry.grapheme = string_value(required(object, "grapheme"));
    validate_grapheme(entry.grapheme);
    entry.case_mode = string_value(required(object, "case"));
    if (entry.case_mode != "exact" && entry.case_mode != "ascii-fold") {
        throw ValidationError("PRONUNCIATION_RESOURCE_SCHEMA",
                              "pronunciation case mode is unknown");
    }
    if (entry.case_mode == "ascii-fold") {
        std::string folded;
        if (!ascii_fold(entry.grapheme, &folded) || folded != entry.grapheme) {
            throw ValidationError("PRONUNCIATION_INVALID_GRAPHEME",
                                  "ascii-fold grapheme must be lowercase ASCII");
        }
    }

    const json::Value::Array &roles = array_value(required(object, "roles"));
    if (roles.empty() || roles.size() > kMaximumRoles) {
        throw ValidationError("PRONUNCIATION_RESOURCE_LIMIT",
                              "pronunciation role count is outside its limit");
    }
    for (const json::Value &role_value : roles) {
        const std::string &role = string_value(role_value);
        if (!stable_role(role)) {
            throw ValidationError("PRONUNCIATION_RESOURCE_SCHEMA",
                                  "pronunciation role is not canonical");
        }
        if (!entry.roles.empty() && entry.roles.back() >= role) {
            throw ValidationError("PRONUNCIATION_RESOURCE_ORDER",
                                  "pronunciation roles must be unique and sorted");
        }
        entry.roles.push_back(role);
    }

    const json::Value::Array &syllables = array_value(required(object, "syllables"));
    if (syllables.empty() || syllables.size() > kMaximumSyllables) {
        throw ValidationError("PRONUNCIATION_RESOURCE_LIMIT",
                              "pronunciation syllable count is outside its limit");
    }
    std::size_t segment_count = 0U;
    std::size_t primary_stresses = 0U;
    for (const json::Value &syllable_value : syllables) {
        const json::Value::Object &syllable_object = object_value(syllable_value);
        if (!exact_keys(syllable_object, {"segments", "stress"})) {
            throw ValidationError("PRONUNCIATION_RESOURCE_SCHEMA",
                                  "syllable has unknown or absent fields");
        }
        PronunciationSyllable syllable;
        syllable.stress =
            parse_stress(string_value(required(syllable_object, "stress")));
        if (syllable.stress == SyllableStress::primary) {
            ++primary_stresses;
            if (primary_stresses > 1U) {
                throw ValidationError("PRONUNCIATION_RESOURCE_SCHEMA",
                                      "pronunciation has multiple primary stresses");
            }
        }
        const json::Value::Array &segments =
            array_value(required(syllable_object, "segments"));
        if (segments.empty() || segments.size() > kMaximumSegmentsPerSyllable) {
            throw ValidationError("PRONUNCIATION_RESOURCE_LIMIT",
                                  "syllable segment count is outside its limit");
        }
        for (const json::Value &segment_value : segments) {
            const std::string &segment = string_value(segment_value);
            const auto found = segment_ids.find(segment);
            if (found == segment_ids.end()) {
                throw ValidationError("PRONUNCIATION_UNKNOWN_SEGMENT",
                                      "pronunciation uses a segment outside the inventory");
            }
            syllable.segment_ids.push_back(found->second);
        }
        segment_count += segments.size();
        if (segment_count > kMaximumSegmentsPerEntry) {
            throw ValidationError("PRONUNCIATION_RESOURCE_LIMIT",
                                  "pronunciation segment count exceeds its limit");
        }
        entry.syllables.push_back(std::move(syllable));
    }

    entry.source = string_value(required(object, "source"));
    if (!stable_identifier(entry.source, 128U)) {
        throw ValidationError("PRONUNCIATION_RESOURCE_SCHEMA",
                              "pronunciation source is not canonical");
    }
    return entry;
}

}  // namespace

const PronunciationEntry *PronunciationLexicon::find(
    std::string_view grapheme, std::string_view role) const {
    if (grapheme.empty() || grapheme.size() > kMaximumGraphemeBytes ||
        (!role.empty() && !stable_role(role))) {
        return nullptr;
    }
    const std::string_view requested_role = role.empty() ? std::string_view("default") : role;
    const auto lookup = [&](char mode, std::string_view key_grapheme,
                            std::string_view key_role) -> const PronunciationEntry * {
        const auto found = index_.find(index_key(mode, key_grapheme, key_role));
        return found == index_.end() ? nullptr : &entries_[found->second];
    };

    if (const PronunciationEntry *entry = lookup('e', grapheme, requested_role)) {
        return entry;
    }
    std::string folded;
    const bool has_fold = ascii_fold(grapheme, &folded);
    if (has_fold) {
        if (const PronunciationEntry *entry = lookup('f', folded, requested_role)) {
            return entry;
        }
    }
    if (requested_role != "default") {
        if (const PronunciationEntry *entry = lookup('e', grapheme, "default")) {
            return entry;
        }
        if (has_fold) {
            return lookup('f', folded, "default");
        }
    }
    return nullptr;
}

const std::string &PronunciationLexicon::resource_id() const noexcept {
    return resource_id_;
}

const std::string &PronunciationLexicon::resource_sha256() const noexcept {
    return resource_sha256_;
}

const std::string &PronunciationLexicon::segment_inventory_sha256() const noexcept {
    return segment_inventory_sha256_;
}

const std::string &PronunciationLexicon::review_record_sha256() const noexcept {
    return review_record_sha256_;
}

PronunciationAdmission PronunciationLexicon::admission() const noexcept {
    return admission_;
}

std::size_t PronunciationLexicon::entry_count() const noexcept {
    return entries_.size();
}

int load_pronunciation_lexicon(
    std::string_view jsonl,
    std::string_view expected_resource_sha256,
    std::string_view expected_segment_inventory_sha256,
    PronunciationAdmission required_admission,
    const std::vector<SegmentDefinition> &segments,
    PronunciationLexicon *lexicon,
    PronunciationResourceFailure *failure) {
    if (lexicon == nullptr || failure == nullptr) {
        return KGV_INVALID_ARGUMENT;
    }
    *lexicon = PronunciationLexicon{};
    *failure = PronunciationResourceFailure{};
    if (!is_lower_sha256(expected_resource_sha256) ||
        !is_lower_sha256(expected_segment_inventory_sha256) || segments.empty()) {
        return reject(lexicon, failure, KGV_INVALID_ARGUMENT,
                      "INVALID_PRONUNCIATION_LOADER_ARGUMENT",
                      "pronunciation loader requires pinned lowercase hashes and segments",
                      0U);
    }

    const std::string canonical_inventory = canonical_segment_inventory(segments);
    if (canonical_inventory.empty()) {
        return reject(lexicon, failure, KGV_INVALID_ARGUMENT,
                      "INVALID_PRONUNCIATION_SEGMENT_INVENTORY",
                      "segment inventory must have unique names and increasing nonzero IDs",
                      0U);
    }
    if (sha256_hex(canonical_inventory) != expected_segment_inventory_sha256) {
        return reject(lexicon, failure, KGV_ABI_MISMATCH,
                      "PRONUNCIATION_SEGMENT_INVENTORY_HASH_MISMATCH",
                      "segment definitions do not match the pinned inventory SHA-256",
                      0U);
    }
    std::map<std::string, std::uint16_t, std::less<>> segment_ids;
    for (const SegmentDefinition &segment : segments) {
        segment_ids.emplace(segment.name, segment.id);
    }
    if (jsonl.empty() || jsonl.size() > kMaximumResourceBytes) {
        return reject(lexicon, failure, KGV_RESOURCE_EXHAUSTED,
                      "PRONUNCIATION_RESOURCE_TOO_LARGE",
                      "pronunciation resource is empty or exceeds 64 MiB", 0U);
    }
    if (sha256_hex(jsonl) != expected_resource_sha256) {
        return reject(lexicon, failure, KGV_HASH_MISMATCH,
                      "PRONUNCIATION_RESOURCE_HASH_MISMATCH",
                      "pronunciation resource does not match its pinned SHA-256", 0U);
    }
    if (jsonl.back() != '\n') {
        return reject(lexicon, failure, KGV_INVALID_MODEL,
                      "PRONUNCIATION_RESOURCE_CANONICAL_FORM",
                      "pronunciation JSONL must end with LF", 0U);
    }

    PronunciationLexicon candidate;
    Header header;
    bool saw_header = false;
    std::size_t parsed_entries = 0U;
    std::size_t cursor = 0U;
    std::size_t line_number = 1U;
    while (cursor < jsonl.size()) {
        const std::size_t end = jsonl.find('\n', cursor);
        if (end == std::string_view::npos) {
            return reject(lexicon, failure, KGV_INVALID_MODEL,
                          "PRONUNCIATION_RESOURCE_CANONICAL_FORM",
                          "pronunciation JSONL has an unterminated line", line_number);
        }
        const std::string_view line = jsonl.substr(cursor, end - cursor);
        if (line.empty() || line.size() > kMaximumLineBytes ||
            line.find('\r') != std::string_view::npos) {
            return reject(lexicon, failure, KGV_INVALID_MODEL,
                          "PRONUNCIATION_RESOURCE_CANONICAL_FORM",
                          "pronunciation JSONL has an empty, oversized, or CR line",
                          line_number);
        }

        json::Value document;
        try {
            document = json::parse(line);
        } catch (const json::ParseError &) {
            return reject(lexicon, failure, KGV_INVALID_MODEL,
                          "PRONUNCIATION_RESOURCE_JSON",
                          "pronunciation resource line is not strict JSON", line_number);
        }
        try {
            if (!saw_header) {
                header = parse_header(document, required_admission,
                                      expected_segment_inventory_sha256);
                saw_header = true;
                candidate.entries_.reserve(header.entry_count);
            } else {
                if (parsed_entries >= header.entry_count) {
                    throw ValidationError("PRONUNCIATION_RESOURCE_ENTRY_COUNT",
                                          "pronunciation resource has extra entries");
                }
                PronunciationEntry entry = parse_entry(document, segment_ids);
                const char mode = entry.case_mode == "exact" ? 'e' : 'f';
                const std::size_t entry_index = candidate.entries_.size();
                for (const std::string &role : entry.roles) {
                    if (!candidate.index_.emplace(
                            index_key(mode, entry.grapheme, role), entry_index).second) {
                        throw ValidationError("PRONUNCIATION_DUPLICATE_ENTRY",
                                              "pronunciation grapheme/role key is duplicated");
                    }
                }
                candidate.entries_.push_back(std::move(entry));
                ++parsed_entries;
            }
        } catch (const ValidationError &error) {
            const int status = error.code() ==
                                       "PRONUNCIATION_RESOURCE_INVENTORY_MISMATCH"
                                   ? KGV_ABI_MISMATCH
                                   : KGV_INVALID_MODEL;
            return reject(lexicon, failure, status, error.code(), error.what(),
                          line_number);
        } catch (const std::exception &) {
            return reject(lexicon, failure, KGV_INVALID_MODEL,
                          "PRONUNCIATION_RESOURCE_SCHEMA",
                          "pronunciation resource line has an invalid value type",
                          line_number);
        }
        cursor = end + 1U;
        ++line_number;
    }

    if (!saw_header || parsed_entries != header.entry_count) {
        return reject(lexicon, failure, KGV_INVALID_MODEL,
                      "PRONUNCIATION_RESOURCE_ENTRY_COUNT",
                      "pronunciation entry count does not match the header", 1U);
    }
    candidate.resource_id_ = std::move(header.resource_id);
    candidate.resource_sha256_ = std::string(expected_resource_sha256);
    candidate.segment_inventory_sha256_ = std::move(header.inventory_sha256);
    candidate.review_record_sha256_ = std::move(header.review_sha256);
    candidate.admission_ = header.admission;
    *lexicon = std::move(candidate);
    return KGV_OK;
}

std::string pronunciation_segment_inventory_sha256(
    const std::vector<SegmentDefinition> &segments) {
    const std::string canonical = canonical_segment_inventory(segments);
    return canonical.empty() ? std::string{} : sha256_hex(canonical);
}

const char *pronunciation_admission_name(PronunciationAdmission value) noexcept {
    switch (value) {
        case PronunciationAdmission::product_admitted: return "product-admitted";
        case PronunciationAdmission::local_user: return "local-user";
        case PronunciationAdmission::test_fixture: return "test-fixture";
    }
    return "unknown";
}

const char *syllable_stress_name(SyllableStress value) noexcept {
    switch (value) {
        case SyllableStress::none: return "none";
        case SyllableStress::primary: return "primary";
        case SyllableStress::secondary: return "secondary";
    }
    return "unknown";
}

}  // namespace kgv
