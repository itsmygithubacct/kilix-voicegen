#include "frontend/tokenization.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
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

constexpr std::size_t kMaximumResourceBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumLineBytes = 64U * 1024U;
constexpr std::size_t kMaximumEntries = 65535U;
constexpr std::size_t kMaximumResolvedWords = 65536U;
constexpr std::size_t kMaximumSyllablesPerWord = 16U;
constexpr std::size_t kMaximumSegmentsPerSyllable = 32U;
constexpr std::size_t kMaximumSegmentsPerWord = 128U;
constexpr std::size_t kMaximumSerializedIds = 4U * 1024U * 1024U;

constexpr std::array<std::string_view, 17U> kRequiredControls = {
    "PAD",
    "BOS",
    "EOS",
    "WB",
    "SYL",
    "STRESS_0",
    "STRESS_1",
    "STRESS_2",
    "END_NONE",
    "END_COMMA",
    "END_COLON",
    "END_SEMICOLON",
    "END_PERIOD",
    "END_QUESTION",
    "END_EXCLAMATION",
    "END_PARAGRAPH",
    "END_CONTINUATION",
};

class ValidationError final : public std::runtime_error {
public:
    ValidationError(std::string code, std::string message)
        : std::runtime_error(std::move(message)), code_(std::move(code)) {}

    const std::string &code() const noexcept { return code_; }

private:
    std::string code_;
};

struct Header final {
    std::string resource_id;
    std::string segment_inventory_sha256;
    std::string frontend_abi_sha256;
    PronunciationAdmission admission = PronunciationAdmission::test_fixture;
    std::size_t entry_count = 0U;
    std::size_t maximum_input_tokens = 0U;
};

struct ParsedEntry final {
    std::uint16_t token_id = 0U;
    bool segment = false;
    std::string name;
    std::uint16_t segment_id = 0U;
};

struct Boundary final {
    PhraseTerminator terminator = PhraseTerminator::none;
    std::uint16_t token_id = 0U;
    int priority = 0;
    SourceSpan span;
};

int reject(ModelTokenInventory *inventory,
           ModelTokenFailure *failure,
           int status,
           std::string code,
           std::string message,
           std::size_t line) {
    if (inventory != nullptr) {
        *inventory = ModelTokenInventory{};
    }
    if (failure != nullptr) {
        failure->status = status;
        failure->code = std::move(code);
        failure->message = std::move(message);
        failure->line = line;
    }
    return status;
}

int reject_result(ModelTokenResult *result,
                  ModelTokenFailure *failure,
                  int status,
                  std::string code,
                  std::string message) {
    if (result != nullptr) {
        *result = ModelTokenResult{};
    }
    if (failure != nullptr) {
        failure->status = status;
        failure->code = std::move(code);
        failure->message = std::move(message);
        failure->line = 0U;
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
        throw ValidationError("MODEL_TOKEN_RESOURCE_SCHEMA",
                              "required model-token field is absent");
    }
    return found->second;
}

const json::Value::Object &object_value(const json::Value &value) {
    if (!value.is_object()) {
        throw ValidationError("MODEL_TOKEN_RESOURCE_SCHEMA",
                              "model-token value must be an object");
    }
    return value.as_object();
}

const std::string &string_value(const json::Value &value) {
    if (!value.is_string()) {
        throw ValidationError("MODEL_TOKEN_RESOURCE_SCHEMA",
                              "model-token value must be a string");
    }
    return value.as_string();
}

std::size_t bounded_count(const json::Value &value,
                          std::size_t minimum,
                          std::size_t maximum,
                          std::string_view description) {
    std::int64_t number = 0;
    try {
        number = value.as_integer();
    } catch (const std::exception &) {
        throw ValidationError("MODEL_TOKEN_RESOURCE_SCHEMA",
                              std::string(description) + " must be an integer");
    }
    if (number < 0) {
        throw ValidationError("MODEL_TOKEN_RESOURCE_LIMIT",
                              std::string(description) + " is outside its limit");
    }
    const std::uint64_t unsigned_number = static_cast<std::uint64_t>(number);
    if (unsigned_number < minimum || unsigned_number > maximum) {
        throw ValidationError("MODEL_TOKEN_RESOURCE_LIMIT",
                              std::string(description) + " is outside its limit");
    }
    return static_cast<std::size_t>(unsigned_number);
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

bool valid_admission(PronunciationAdmission admission) noexcept {
    return admission == PronunciationAdmission::product_admitted ||
           admission == PronunciationAdmission::local_user ||
           admission == PronunciationAdmission::test_fixture;
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
    throw ValidationError("MODEL_TOKEN_RESOURCE_ADMISSION",
                          "model-token admission is unknown");
}

bool required_control(std::string_view name) noexcept {
    return std::find(kRequiredControls.begin(), kRequiredControls.end(), name) !=
           kRequiredControls.end();
}

Header parse_header(const json::Value &document,
                    PronunciationAdmission required_admission,
                    std::string_view expected_inventory_sha256,
                    std::size_t segment_count) {
    const json::Value::Object &object = object_value(document);
    if (!exact_keys(object,
                    {"admission", "dialect", "entry_count",
                     "frontend_abi_sha256", "maximum_input_tokens",
                     "resource_id", "schema", "segment_inventory_sha256"})) {
        throw ValidationError("MODEL_TOKEN_RESOURCE_SCHEMA",
                              "model-token header has unknown or absent fields");
    }
    if (string_value(required(object, "schema")) !=
        "kilix.voicegen.model-token-inventory/v1") {
        throw ValidationError("MODEL_TOKEN_RESOURCE_SCHEMA",
                              "model-token header schema is unsupported");
    }
    if (string_value(required(object, "dialect")) != "en-AU") {
        throw ValidationError("MODEL_TOKEN_RESOURCE_DIALECT",
                              "model-token dialect must be en-AU");
    }
    Header header;
    header.admission =
        parse_admission(string_value(required(object, "admission")));
    if (header.admission != required_admission) {
        throw ValidationError("MODEL_TOKEN_RESOURCE_ADMISSION",
                              "model-token admission does not match the caller");
    }
    header.resource_id = string_value(required(object, "resource_id"));
    if (!stable_identifier(header.resource_id, 128U)) {
        throw ValidationError("MODEL_TOKEN_RESOURCE_SCHEMA",
                              "model-token resource ID is not canonical");
    }
    header.segment_inventory_sha256 =
        string_value(required(object, "segment_inventory_sha256"));
    if (!is_lower_sha256(header.segment_inventory_sha256)) {
        throw ValidationError("MODEL_TOKEN_RESOURCE_SCHEMA",
                              "segment inventory binding is not lowercase SHA-256");
    }
    if (header.segment_inventory_sha256 != expected_inventory_sha256) {
        throw ValidationError("MODEL_TOKEN_RESOURCE_INVENTORY_MISMATCH",
                              "model-token resource targets another segment inventory");
    }
    header.frontend_abi_sha256 =
        string_value(required(object, "frontend_abi_sha256"));
    if (!is_lower_sha256(header.frontend_abi_sha256)) {
        throw ValidationError("MODEL_TOKEN_RESOURCE_SCHEMA",
                              "frontend ABI binding is not lowercase SHA-256");
    }
    if (segment_count > kMaximumEntries - kRequiredControls.size()) {
        throw ValidationError("MODEL_TOKEN_RESOURCE_LIMIT",
                              "segment inventory is too large for token IDs");
    }
    const std::size_t required_entries = segment_count + kRequiredControls.size();
    header.entry_count = bounded_count(required(object, "entry_count"),
                                       required_entries, required_entries,
                                       "model-token entry count");
    header.maximum_input_tokens = bounded_count(
        required(object, "maximum_input_tokens"), 8U, 65535U,
        "model maximum input tokens");
    return header;
}

ParsedEntry parse_entry(
    const json::Value &document,
    const std::map<std::string, std::uint16_t, std::less<>> &segments_by_name) {
    const json::Value::Object &object = object_value(document);
    if (!exact_keys(object, {"id", "kind", "name", "schema", "segment_id"})) {
        throw ValidationError("MODEL_TOKEN_RESOURCE_SCHEMA",
                              "model-token entry has unknown or absent fields");
    }
    if (string_value(required(object, "schema")) !=
        "kilix.voicegen.model-token-entry/v1") {
        throw ValidationError("MODEL_TOKEN_RESOURCE_SCHEMA",
                              "model-token entry schema is unsupported");
    }
    ParsedEntry entry;
    entry.token_id = static_cast<std::uint16_t>(bounded_count(
        required(object, "id"), 0U,
        static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()),
        "model token ID"));
    entry.name = string_value(required(object, "name"));
    const std::string &kind = string_value(required(object, "kind"));
    const json::Value &segment_id = required(object, "segment_id");
    if (kind == "control") {
        if (!required_control(entry.name) || !segment_id.is_null()) {
            throw ValidationError("MODEL_TOKEN_RESOURCE_SCHEMA",
                                  "control token name or segment binding is invalid");
        }
        return entry;
    }
    if (kind != "segment" || segment_id.is_null()) {
        throw ValidationError("MODEL_TOKEN_RESOURCE_SCHEMA",
                              "model-token kind or segment binding is invalid");
    }
    entry.segment = true;
    const auto expected = segments_by_name.find(entry.name);
    if (expected == segments_by_name.end()) {
        throw ValidationError("MODEL_TOKEN_UNKNOWN_SEGMENT",
                              "model token names a segment outside the inventory");
    }
    entry.segment_id = static_cast<std::uint16_t>(bounded_count(
        segment_id, 1U,
        static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()),
        "model segment ID"));
    if (entry.segment_id != expected->second) {
        throw ValidationError("MODEL_TOKEN_RESOURCE_INVENTORY_MISMATCH",
                              "model token binds a segment to the wrong ID");
    }
    return entry;
}

std::optional<std::pair<std::string_view, int>> terminator_control(
    PhraseTerminator terminator) noexcept {
    switch (terminator) {
        case PhraseTerminator::none: return std::pair{"END_NONE", 3};
        case PhraseTerminator::comma: return std::pair{"END_COMMA", 1};
        case PhraseTerminator::colon: return std::pair{"END_COLON", 2};
        case PhraseTerminator::semicolon:
            return std::pair{"END_SEMICOLON", 2};
        case PhraseTerminator::period: return std::pair{"END_PERIOD", 3};
        case PhraseTerminator::question:
            return std::pair{"END_QUESTION", 3};
        case PhraseTerminator::exclamation:
            return std::pair{"END_EXCLAMATION", 3};
        case PhraseTerminator::paragraph:
            return std::pair{"END_PARAGRAPH", 4};
        case PhraseTerminator::continuation:
            return std::pair{"END_CONTINUATION", 5};
    }
    return std::nullopt;
}

std::string_view stress_control(SyllableStress stress) noexcept {
    switch (stress) {
        case SyllableStress::none: return "STRESS_0";
        case SyllableStress::primary: return "STRESS_1";
        case SyllableStress::secondary: return "STRESS_2";
    }
    return {};
}

}  // namespace

std::optional<std::uint16_t> ModelTokenInventory::control_id(
    std::string_view name) const noexcept {
    const auto found = controls_.find(name);
    return found == controls_.end()
               ? std::optional<std::uint16_t>{}
               : std::optional<std::uint16_t>{found->second};
}

std::optional<std::uint16_t> ModelTokenInventory::segment_token_id(
    std::uint16_t segment_id) const noexcept {
    const auto found = segment_tokens_.find(segment_id);
    return found == segment_tokens_.end()
               ? std::optional<std::uint16_t>{}
               : std::optional<std::uint16_t>{found->second};
}

const std::string &ModelTokenInventory::resource_id() const noexcept {
    return resource_id_;
}

const std::string &ModelTokenInventory::resource_sha256() const noexcept {
    return resource_sha256_;
}

const std::string &ModelTokenInventory::segment_inventory_sha256() const noexcept {
    return segment_inventory_sha256_;
}

const std::string &ModelTokenInventory::frontend_abi_sha256() const noexcept {
    return frontend_abi_sha256_;
}

PronunciationAdmission ModelTokenInventory::admission() const noexcept {
    return admission_;
}

std::size_t ModelTokenInventory::entry_count() const noexcept {
    return controls_.size() + segment_tokens_.size();
}

std::size_t ModelTokenInventory::maximum_input_tokens() const noexcept {
    return maximum_input_tokens_;
}

int load_model_token_inventory(
    std::string_view jsonl,
    std::string_view expected_resource_sha256,
    std::string_view expected_segment_inventory_sha256,
    PronunciationAdmission required_admission,
    const std::vector<SegmentDefinition> &segments,
    ModelTokenInventory *inventory,
    ModelTokenFailure *failure) {
    if (inventory == nullptr || failure == nullptr) {
        return reject(inventory, failure, KGV_INVALID_ARGUMENT,
                      "INVALID_MODEL_TOKEN_LOADER_ARGUMENT",
                      "model-token loader requires output records", 0U);
    }
    *inventory = ModelTokenInventory{};
    *failure = ModelTokenFailure{};
    if (!is_lower_sha256(expected_resource_sha256) ||
        !is_lower_sha256(expected_segment_inventory_sha256) ||
        !valid_admission(required_admission) || segments.empty()) {
        return reject(inventory, failure, KGV_INVALID_ARGUMENT,
                      "INVALID_MODEL_TOKEN_LOADER_ARGUMENT",
                      "model-token loader requires pinned hashes, admission, and segments",
                      0U);
    }
    const std::string actual_inventory_sha256 =
        pronunciation_segment_inventory_sha256(segments);
    if (actual_inventory_sha256.empty()) {
        return reject(inventory, failure, KGV_INVALID_ARGUMENT,
                      "INVALID_MODEL_TOKEN_SEGMENT_INVENTORY",
                      "model-token segment inventory is not canonical", 0U);
    }
    if (actual_inventory_sha256 != expected_segment_inventory_sha256) {
        return reject(inventory, failure, KGV_ABI_MISMATCH,
                      "MODEL_TOKEN_SEGMENT_INVENTORY_HASH_MISMATCH",
                      "segment definitions do not match the pinned inventory",
                      0U);
    }
    if (jsonl.empty() || jsonl.size() > kMaximumResourceBytes) {
        return reject(inventory, failure, KGV_RESOURCE_EXHAUSTED,
                      "MODEL_TOKEN_RESOURCE_TOO_LARGE",
                      "model-token resource is empty or exceeds 16 MiB", 0U);
    }
    if (sha256_hex(jsonl) != expected_resource_sha256) {
        return reject(inventory, failure, KGV_HASH_MISMATCH,
                      "MODEL_TOKEN_RESOURCE_HASH_MISMATCH",
                      "model-token resource does not match its pinned SHA-256", 0U);
    }
    if (jsonl.back() != '\n') {
        return reject(inventory, failure, KGV_INVALID_MODEL,
                      "MODEL_TOKEN_RESOURCE_CANONICAL_FORM",
                      "model-token JSONL must end with LF", 0U);
    }

    std::map<std::string, std::uint16_t, std::less<>> segments_by_name;
    for (const SegmentDefinition &segment : segments) {
        segments_by_name.emplace(segment.name, segment.id);
    }
    Header header;
    bool saw_header = false;
    bool saw_token = false;
    std::uint16_t previous_token_id = 0U;
    std::set<std::string, std::less<>> names;
    std::map<std::string, std::uint16_t, std::less<>> controls;
    std::map<std::uint16_t, std::uint16_t> segment_tokens;
    std::size_t parsed_entries = 0U;
    std::size_t cursor = 0U;
    std::size_t line_number = 1U;
    while (cursor < jsonl.size()) {
        const std::size_t end = jsonl.find('\n', cursor);
        if (end == std::string_view::npos) {
            return reject(inventory, failure, KGV_INVALID_MODEL,
                          "MODEL_TOKEN_RESOURCE_CANONICAL_FORM",
                          "model-token JSONL contains an unterminated line",
                          line_number);
        }
        const std::string_view line = jsonl.substr(cursor, end - cursor);
        if (line.empty() || line.size() > kMaximumLineBytes ||
            line.find('\r') != std::string_view::npos) {
            return reject(inventory, failure, KGV_INVALID_MODEL,
                          "MODEL_TOKEN_RESOURCE_CANONICAL_FORM",
                          "model-token JSONL has an empty, oversized, or CR line",
                          line_number);
        }
        json::Value document;
        try {
            document = json::parse(line);
        } catch (const json::ParseError &) {
            return reject(inventory, failure, KGV_INVALID_MODEL,
                          "MODEL_TOKEN_RESOURCE_JSON",
                          "model-token line is not strict JSON", line_number);
        }
        try {
            if (!saw_header) {
                header = parse_header(document, required_admission,
                                      expected_segment_inventory_sha256,
                                      segments.size());
                saw_header = true;
            } else {
                if (parsed_entries >= header.entry_count) {
                    throw ValidationError("MODEL_TOKEN_RESOURCE_ENTRY_COUNT",
                                          "model-token resource has extra entries");
                }
                ParsedEntry entry = parse_entry(document, segments_by_name);
                if ((saw_token && entry.token_id <= previous_token_id) ||
                    !names.insert(entry.name).second) {
                    throw ValidationError("MODEL_TOKEN_RESOURCE_ORDER",
                                          "model-token IDs and names must be unique and ordered");
                }
                if (entry.segment) {
                    if (!segment_tokens.emplace(entry.segment_id,
                                                entry.token_id).second) {
                        throw ValidationError("MODEL_TOKEN_RESOURCE_INVENTORY_MISMATCH",
                                              "segment has multiple model token IDs");
                    }
                } else {
                    controls.emplace(std::move(entry.name), entry.token_id);
                }
                previous_token_id = entry.token_id;
                saw_token = true;
                ++parsed_entries;
            }
        } catch (const ValidationError &error) {
            const int status =
                error.code() == "MODEL_TOKEN_RESOURCE_INVENTORY_MISMATCH"
                    ? KGV_ABI_MISMATCH
                    : KGV_INVALID_MODEL;
            return reject(inventory, failure, status, error.code(), error.what(),
                          line_number);
        } catch (const std::exception &) {
            return reject(inventory, failure, KGV_INVALID_MODEL,
                          "MODEL_TOKEN_RESOURCE_SCHEMA",
                          "model-token line has an invalid value type",
                          line_number);
        }
        cursor = end + 1U;
        ++line_number;
    }
    if (!saw_header || parsed_entries != header.entry_count) {
        return reject(inventory, failure, KGV_INVALID_MODEL,
                      "MODEL_TOKEN_RESOURCE_ENTRY_COUNT",
                      "model-token entry count does not match the header", 1U);
    }
    if (controls.size() != kRequiredControls.size() ||
        segment_tokens.size() != segments.size()) {
        return reject(inventory, failure, KGV_INVALID_MODEL,
                      "MODEL_TOKEN_RESOURCE_COMPLETENESS",
                      "model-token resource does not cover all controls and segments",
                      1U);
    }
    for (std::string_view control : kRequiredControls) {
        if (controls.find(control) == controls.end()) {
            return reject(inventory, failure, KGV_INVALID_MODEL,
                          "MODEL_TOKEN_RESOURCE_COMPLETENESS",
                          "model-token resource lacks a required control", 1U);
        }
    }

    ModelTokenInventory candidate;
    candidate.resource_id_ = std::move(header.resource_id);
    candidate.resource_sha256_ = std::string(expected_resource_sha256);
    candidate.segment_inventory_sha256_ =
        std::move(header.segment_inventory_sha256);
    candidate.frontend_abi_sha256_ = std::move(header.frontend_abi_sha256);
    candidate.admission_ = header.admission;
    candidate.maximum_input_tokens_ = header.maximum_input_tokens;
    candidate.controls_ = std::move(controls);
    candidate.segment_tokens_ = std::move(segment_tokens);
    *inventory = std::move(candidate);
    return KGV_OK;
}

int serialize_model_tokens(
    const ModelTokenInventory &inventory,
    std::size_t input_bytes,
    const std::vector<ResolvedTokenWord> &words,
    const std::vector<ResolvedTokenPhrase> &phrases,
    ModelTokenResult *result,
    ModelTokenFailure *failure) {
    if (result == nullptr || failure == nullptr) {
        return reject_result(result, failure, KGV_INVALID_ARGUMENT,
                             "INVALID_MODEL_TOKEN_SERIALIZER_ARGUMENT",
                             "model-token serializer requires output records");
    }
    *result = ModelTokenResult{};
    *failure = ModelTokenFailure{};
    if (inventory.resource_sha256().empty() ||
        inventory.maximum_input_tokens() < 8U) {
        return reject_result(result, failure, KGV_INVALID_STATE,
                             "MODEL_TOKEN_INVENTORY_NOT_LOADED",
                             "model-token inventory is not loaded");
    }
    if (input_bytes > KGV_MAX_INPUT_BYTES || words.size() > kMaximumResolvedWords) {
        return reject_result(result, failure, KGV_INVALID_ARGUMENT,
                             "MODEL_TOKEN_INPUT_LIMIT",
                             "resolved frontend input exceeds its limit");
    }
    result->inventory_sha256 = inventory.resource_sha256();
    if (words.empty()) {
        if (!phrases.empty()) {
            return reject_result(result, failure, KGV_INVALID_ARGUMENT,
                                 "MODEL_TOKEN_INVALID_PHRASES",
                                 "empty word input must have no phrases");
        }
        return KGV_OK;
    }
    if (phrases.empty() || phrases.size() > words.size()) {
        return reject_result(result, failure, KGV_INVALID_ARGUMENT,
                             "MODEL_TOKEN_INVALID_PHRASES",
                             "resolved words require bounded phrase coverage");
    }

    const auto bos = inventory.control_id("BOS");
    const auto eos = inventory.control_id("EOS");
    const auto wb = inventory.control_id("WB");
    const auto syl = inventory.control_id("SYL");
    const auto continuation_id = inventory.control_id("END_CONTINUATION");
    if (!bos || !eos || !wb || !syl || !continuation_id) {
        return reject_result(result, failure, KGV_INVALID_STATE,
                             "MODEL_TOKEN_INVENTORY_NOT_LOADED",
                             "model-token inventory lacks required controls");
    }

    std::vector<std::vector<std::uint16_t>> atoms;
    atoms.reserve(words.size());
    std::size_t total_atom_ids = 0U;
    std::size_t previous_start = 0U;
    bool saw_word = false;
    for (const ResolvedTokenWord &word : words) {
        if (word.span.byte_start >= word.span.byte_end ||
            word.span.byte_end > input_bytes ||
            (saw_word && word.span.byte_start < previous_start) ||
            word.syllables.empty() ||
            word.syllables.size() > kMaximumSyllablesPerWord) {
            return reject_result(result, failure, KGV_INVALID_ARGUMENT,
                                 "MODEL_TOKEN_INVALID_WORD",
                                 "resolved word span or syllable count is invalid");
        }
        std::vector<std::uint16_t> atom;
        atom.push_back(*wb);
        std::size_t segment_count = 0U;
        std::size_t primary_count = 0U;
        for (const PronunciationSyllable &syllable : word.syllables) {
            if (syllable.segment_ids.empty() ||
                syllable.segment_ids.size() > kMaximumSegmentsPerSyllable) {
                return reject_result(result, failure, KGV_INVALID_ARGUMENT,
                                     "MODEL_TOKEN_INVALID_WORD",
                                     "resolved syllable segment count is invalid");
            }
            const std::string_view stress_name = stress_control(syllable.stress);
            const auto stress = inventory.control_id(stress_name);
            if (stress_name.empty() || !stress) {
                return reject_result(result, failure, KGV_ABI_MISMATCH,
                                     "MODEL_TOKEN_UNKNOWN_STRESS",
                                     "resolved stress lacks a model token");
            }
            if (syllable.stress == SyllableStress::primary &&
                ++primary_count > 1U) {
                return reject_result(result, failure, KGV_INVALID_ARGUMENT,
                                     "MODEL_TOKEN_INVALID_WORD",
                                     "resolved word has multiple primary stresses");
            }
            segment_count += syllable.segment_ids.size();
            if (segment_count > kMaximumSegmentsPerWord) {
                return reject_result(result, failure, KGV_INVALID_ARGUMENT,
                                     "MODEL_TOKEN_INVALID_WORD",
                                     "resolved word exceeds its segment limit");
            }
            atom.push_back(*syl);
            atom.push_back(*stress);
            for (std::uint16_t segment_id : syllable.segment_ids) {
                const auto token_id = inventory.segment_token_id(segment_id);
                if (!token_id) {
                    return reject_result(result, failure, KGV_ABI_MISMATCH,
                                         "MODEL_TOKEN_UNKNOWN_SEGMENT",
                                         "resolved segment lacks a model token");
                }
                atom.push_back(*token_id);
            }
        }
        total_atom_ids += atom.size();
        if (total_atom_ids > kMaximumSerializedIds) {
            return reject_result(result, failure, KGV_RESOURCE_EXHAUSTED,
                                 "MODEL_TOKEN_OUTPUT_LIMIT",
                                 "serialized word atoms exceed the global limit");
        }
        atoms.push_back(std::move(atom));
        previous_start = word.span.byte_start;
        saw_word = true;
    }

    std::vector<std::optional<Boundary>> boundaries(words.size() + 1U);
    std::array<std::vector<std::size_t>, 6U> boundary_ends_by_priority;
    std::size_t expected_word_start = 0U;
    std::size_t previous_phrase_start = 0U;
    bool saw_phrase = false;
    for (const ResolvedTokenPhrase &phrase : phrases) {
        if (phrase.word_start != expected_word_start ||
            phrase.word_end <= phrase.word_start ||
            phrase.word_end > words.size() ||
            phrase.span.byte_start >= phrase.span.byte_end ||
            phrase.span.byte_end > input_bytes ||
            (saw_phrase && phrase.span.byte_start < previous_phrase_start) ||
            phrase.span.byte_start > words[phrase.word_start].span.byte_start ||
            phrase.span.byte_end < words[phrase.word_end - 1U].span.byte_end) {
            return reject_result(result, failure, KGV_INVALID_ARGUMENT,
                                 "MODEL_TOKEN_INVALID_PHRASES",
                                 "resolved phrases must cover words contiguously");
        }
        const auto control = terminator_control(phrase.terminator);
        if (!control) {
            return reject_result(result, failure, KGV_INVALID_ARGUMENT,
                                 "MODEL_TOKEN_INVALID_PHRASES",
                                 "resolved phrase terminator is invalid");
        }
        const auto token_id = inventory.control_id(control->first);
        if (!token_id) {
            return reject_result(result, failure, KGV_ABI_MISMATCH,
                                 "MODEL_TOKEN_UNKNOWN_TERMINATOR",
                                 "phrase terminator lacks a model token");
        }
        Boundary boundary;
        boundary.terminator = phrase.terminator;
        boundary.token_id = *token_id;
        boundary.priority = control->second;
        boundary.span = phrase.span;
        boundaries[phrase.word_end] = boundary;
        boundary_ends_by_priority[static_cast<std::size_t>(boundary.priority)]
            .push_back(phrase.word_end);
        expected_word_start = phrase.word_end;
        previous_phrase_start = phrase.span.byte_start;
        saw_phrase = true;
    }
    if (expected_word_start != words.size()) {
        return reject_result(result, failure, KGV_INVALID_ARGUMENT,
                             "MODEL_TOKEN_INVALID_PHRASES",
                             "resolved phrases do not cover every word");
    }

    std::vector<std::size_t> atom_prefix(words.size() + 1U, 0U);
    std::vector<std::size_t> boundary_prefix(words.size() + 1U, 0U);
    for (std::size_t index = 0U; index < words.size(); ++index) {
        atom_prefix[index + 1U] = atom_prefix[index] + atoms[index].size();
        boundary_prefix[index + 1U] =
            boundary_prefix[index] + (boundaries[index + 1U] ? 1U : 0U);
    }
    const auto range_size = [&](std::size_t start, std::size_t end) {
        const std::size_t atoms_size = atom_prefix[end] - atom_prefix[start];
        const std::size_t boundary_size =
            boundary_prefix[end] - boundary_prefix[start];
        const std::size_t forced_terminator = boundaries[end] ? 0U : 1U;
        return 2U + atoms_size + boundary_size + forced_terminator;
    };

    bool starts_continuation = false;
    std::size_t start = 0U;
    std::size_t total_chunk_ids = 0U;
    while (start < words.size()) {
        std::size_t hard_end = words.size();
        const std::vector<std::size_t> &continuations =
            boundary_ends_by_priority[5U];
        const auto next_hard = std::upper_bound(continuations.begin(),
                                                continuations.end(), start);
        if (next_hard != continuations.end()) {
            hard_end = *next_hard;
        }

        std::size_t end = 0U;
        bool forced = false;
        if (range_size(start, hard_end) <= inventory.maximum_input_tokens()) {
            end = hard_end;
        } else {
            std::size_t low = start + 1U;
            std::size_t high = hard_end;
            std::size_t furthest = start;
            while (low <= high) {
                const std::size_t middle = low + (high - low) / 2U;
                if (range_size(start, middle) <=
                    inventory.maximum_input_tokens()) {
                    furthest = middle;
                    low = middle + 1U;
                } else {
                    high = middle - 1U;
                }
            }
            if (furthest == start) {
                return reject_result(result, failure, KGV_RESOURCE_EXHAUSTED,
                                     "MODEL_INPUT_ATOM_TOO_LARGE",
                                     "one resolved word exceeds the model token budget");
            }
            for (int priority = 5; priority >= 1 && end == 0U; --priority) {
                const std::vector<std::size_t> &candidate_ends =
                    boundary_ends_by_priority[static_cast<std::size_t>(priority)];
                const auto after = std::upper_bound(candidate_ends.begin(),
                                                    candidate_ends.end(), furthest);
                if (after != candidate_ends.begin()) {
                    const std::size_t candidate = *std::prev(after);
                    if (candidate > start) {
                        end = candidate;
                    }
                }
            }
            if (end == 0U) {
                end = furthest;
                forced = true;
            }
        }

        ModelTokenChunk chunk;
        chunk.ids.reserve(range_size(start, end));
        chunk.ids.push_back(*bos);
        for (std::size_t word_index = start; word_index < end; ++word_index) {
            chunk.ids.insert(chunk.ids.end(), atoms[word_index].begin(),
                             atoms[word_index].end());
            if (boundaries[word_index + 1U]) {
                chunk.ids.push_back(boundaries[word_index + 1U]->token_id);
            }
        }
        if (forced) {
            chunk.ids.push_back(*continuation_id);
        }
        chunk.ids.push_back(*eos);
        if (chunk.ids.size() > inventory.maximum_input_tokens()) {
            return reject_result(result, failure, KGV_INTERNAL_ERROR,
                                 "MODEL_TOKEN_CHUNK_INVARIANT",
                                 "serialized chunk exceeded its verified budget");
        }
        const bool boundary_continuation =
            !forced && boundaries[end] &&
            boundaries[end]->terminator == PhraseTerminator::continuation;
        chunk.continuation =
            starts_continuation || forced || boundary_continuation;
        chunk.source_span.byte_start = words[start].span.byte_start;
        chunk.source_span.byte_end = forced
                                         ? words[end - 1U].span.byte_end
                                         : std::max(words[end - 1U].span.byte_end,
                                                    boundaries[end]->span.byte_end);
        total_chunk_ids += chunk.ids.size();
        if (total_chunk_ids > kMaximumSerializedIds) {
            return reject_result(result, failure, KGV_RESOURCE_EXHAUSTED,
                                 "MODEL_TOKEN_OUTPUT_LIMIT",
                                 "serialized chunks exceed the global limit");
        }
        result->chunks.push_back(std::move(chunk));
        starts_continuation = forced || boundary_continuation;
        start = end;
    }
    return KGV_OK;
}

std::string model_token_result_json(const ModelTokenResult &result) {
    std::string output = "{\"chunks\":[";
    for (std::size_t index = 0U; index < result.chunks.size(); ++index) {
        if (index != 0U) output.push_back(',');
        const ModelTokenChunk &chunk = result.chunks[index];
        output += "{\"continuation\":";
        output += chunk.continuation ? "true" : "false";
        output += ",\"ids\":[";
        for (std::size_t token_index = 0U; token_index < chunk.ids.size();
             ++token_index) {
            if (token_index != 0U) output.push_back(',');
            output += std::to_string(chunk.ids[token_index]);
        }
        output += "],\"source_byte_end\":";
        output += std::to_string(chunk.source_span.byte_end);
        output += ",\"source_byte_start\":";
        output += std::to_string(chunk.source_span.byte_start);
        output.push_back('}');
    }
    output += "],\"dialect\":\"en-AU\",\"inventory_sha256\":\"";
    output += result.inventory_sha256;
    output += "\",\"schema\":\"kilix.voicegen.tokens/v1\"}\n";
    return output;
}

}  // namespace kgv
