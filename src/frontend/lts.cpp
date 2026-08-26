#include "frontend/lts.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
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

constexpr std::size_t kMaximumResourceBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaximumLineBytes = 64U * 1024U;
constexpr std::size_t kMaximumNodes = 500000U;
constexpr std::size_t kMaximumContext = 8U;
constexpr std::size_t kMaximumSteps = 64U;
constexpr std::size_t kMaximumGraphemeBytes = 256U;
constexpr std::size_t kMaximumEmissionsPerLeaf = 4U;
constexpr std::size_t kMaximumSegmentsPerEmission = 16U;
constexpr std::size_t kMaximumSegmentsPerLeaf = 32U;
constexpr std::size_t kMaximumSyllablesPerWord = 16U;
constexpr std::size_t kMaximumSegmentsPerWord = 128U;

class ValidationError final : public std::runtime_error {
public:
    ValidationError(std::string code, std::string message)
        : std::runtime_error(std::move(message)), code_(std::move(code)) {}

    const std::string &code() const noexcept { return code_; }

private:
    std::string code_;
};

struct ParsedEmission final {
    std::vector<std::uint16_t> segment_ids;
    bool ends_syllable = false;
    SyllableStress stress = SyllableStress::none;
};

struct ParsedNode final {
    bool leaf = false;
    std::int8_t feature_offset = 0;
    char feature_value = '\0';
    std::uint32_t yes = 0U;
    std::uint32_t no = 0U;
    std::vector<ParsedEmission> emissions;
};

struct Header final {
    std::string resource_id;
    std::string segment_inventory_sha256;
    std::string source_lexicon_sha256;
    std::string training_record_sha256;
    std::string review_record_sha256;
    PronunciationAdmission admission = PronunciationAdmission::test_fixture;
    std::uint32_t context_left = 0U;
    std::uint32_t context_right = 0U;
    std::uint32_t maximum_steps = 0U;
    std::size_t node_count = 0U;
    std::map<char, std::uint32_t> roots;
};

int reject(LtsModel *model,
           LtsFailure *failure,
           int status,
           std::string code,
           std::string message,
           std::size_t line) {
    if (model != nullptr) {
        *model = LtsModel{};
    }
    if (failure != nullptr) {
        failure->status = status;
        failure->code = std::move(code);
        failure->message = std::move(message);
        failure->line = line;
    }
    return status;
}

int reject_pronunciation(std::vector<PronunciationSyllable> *syllables,
                         LtsFailure *failure,
                         int status,
                         std::string code,
                         std::string message) {
    if (syllables != nullptr) {
        syllables->clear();
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
        throw ValidationError("LTS_RESOURCE_SCHEMA",
                              "required LTS resource field is absent");
    }
    return found->second;
}

const json::Value::Object &object_value(const json::Value &value) {
    if (!value.is_object()) {
        throw ValidationError("LTS_RESOURCE_SCHEMA",
                              "LTS resource value must be an object");
    }
    return value.as_object();
}

const json::Value::Array &array_value(const json::Value &value) {
    if (!value.is_array()) {
        throw ValidationError("LTS_RESOURCE_SCHEMA",
                              "LTS resource value must be an array");
    }
    return value.as_array();
}

const std::string &string_value(const json::Value &value) {
    if (!value.is_string()) {
        throw ValidationError("LTS_RESOURCE_SCHEMA",
                              "LTS resource value must be a string");
    }
    return value.as_string();
}

std::int64_t integer_value(const json::Value &value,
                           std::string_view description) {
    try {
        return value.as_integer();
    } catch (const std::exception &) {
        throw ValidationError("LTS_RESOURCE_SCHEMA",
                              std::string(description) + " must be an integer");
    }
}

std::size_t bounded_count(const json::Value &value,
                          std::size_t minimum,
                          std::size_t maximum,
                          std::string_view description) {
    const std::int64_t number = integer_value(value, description);
    if (number < 0) {
        throw ValidationError("LTS_RESOURCE_LIMIT",
                              std::string(description) + " is outside its limit");
    }
    const std::uint64_t unsigned_number = static_cast<std::uint64_t>(number);
    if (unsigned_number < minimum || unsigned_number > maximum) {
        throw ValidationError("LTS_RESOURCE_LIMIT",
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

bool word_symbol(char value) noexcept {
    return (value >= 'a' && value <= 'z') || value == '\'' || value == '-';
}

bool feature_symbol(char value) noexcept {
    return word_symbol(value) || value == '^' || value == '$';
}

bool canonical_word(std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaximumGraphemeBytes ||
        value.front() < 'a' || value.front() > 'z' ||
        value.back() < 'a' || value.back() > 'z') {
        return false;
    }
    for (std::size_t index = 0U; index < value.size(); ++index) {
        const char symbol = value[index];
        if (!word_symbol(symbol)) {
            return false;
        }
        if ((symbol == '\'' || symbol == '-') &&
            (index == 0U || index + 1U == value.size() ||
             value[index - 1U] < 'a' || value[index - 1U] > 'z' ||
             value[index + 1U] < 'a' || value[index + 1U] > 'z')) {
            return false;
        }
    }
    return true;
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
    throw ValidationError("LTS_RESOURCE_ADMISSION",
                          "LTS resource admission is unknown");
}

SyllableStress parse_stress(std::string_view value) {
    if (value == "none") return SyllableStress::none;
    if (value == "primary") return SyllableStress::primary;
    if (value == "secondary") return SyllableStress::secondary;
    throw ValidationError("LTS_RESOURCE_SCHEMA",
                          "LTS syllable stress is unknown");
}

Header parse_header(const json::Value &document,
                    PronunciationAdmission required_admission,
                    std::string_view expected_inventory_sha256) {
    const json::Value::Object &object = object_value(document);
    if (!exact_keys(object,
                    {"admission", "context_left", "context_right", "dialect",
                     "maximum_steps", "node_count", "resource_id",
                     "review_record_sha256", "roots", "schema",
                     "segment_inventory_sha256", "source_lexicon_sha256",
                     "training_record_sha256"})) {
        throw ValidationError("LTS_RESOURCE_SCHEMA",
                              "LTS resource header has unknown or absent fields");
    }
    if (string_value(required(object, "schema")) !=
        "kilix.voicegen.lts-model/v1") {
        throw ValidationError("LTS_RESOURCE_SCHEMA",
                              "LTS resource header schema is unsupported");
    }
    if (string_value(required(object, "dialect")) != "en-AU") {
        throw ValidationError("LTS_RESOURCE_DIALECT",
                              "LTS resource dialect must be en-AU");
    }

    Header header;
    header.admission =
        parse_admission(string_value(required(object, "admission")));
    if (header.admission != required_admission) {
        throw ValidationError("LTS_RESOURCE_ADMISSION",
                              "LTS resource admission does not match the caller");
    }
    header.resource_id = string_value(required(object, "resource_id"));
    if (!stable_identifier(header.resource_id, 128U)) {
        throw ValidationError("LTS_RESOURCE_SCHEMA",
                              "LTS resource ID is not canonical");
    }
    header.segment_inventory_sha256 =
        string_value(required(object, "segment_inventory_sha256"));
    if (!is_lower_sha256(header.segment_inventory_sha256)) {
        throw ValidationError("LTS_RESOURCE_SCHEMA",
                              "LTS segment inventory hash is not lowercase SHA-256");
    }
    if (header.segment_inventory_sha256 != expected_inventory_sha256) {
        throw ValidationError("LTS_RESOURCE_INVENTORY_MISMATCH",
                              "LTS resource targets another segment inventory");
    }
    header.source_lexicon_sha256 =
        string_value(required(object, "source_lexicon_sha256"));
    header.training_record_sha256 =
        string_value(required(object, "training_record_sha256"));
    if (!is_lower_sha256(header.source_lexicon_sha256) ||
        !is_lower_sha256(header.training_record_sha256)) {
        throw ValidationError("LTS_RESOURCE_SCHEMA",
                              "LTS source and training bindings must be lowercase SHA-256");
    }

    header.context_left = static_cast<std::uint32_t>(bounded_count(
        required(object, "context_left"), 0U, kMaximumContext,
        "LTS left context"));
    header.context_right = static_cast<std::uint32_t>(bounded_count(
        required(object, "context_right"), 0U, kMaximumContext,
        "LTS right context"));
    header.maximum_steps = static_cast<std::uint32_t>(bounded_count(
        required(object, "maximum_steps"), 1U, kMaximumSteps,
        "LTS maximum steps"));
    header.node_count = bounded_count(required(object, "node_count"), 1U,
                                      kMaximumNodes, "LTS node count");

    const json::Value &review = required(object, "review_record_sha256");
    if (header.admission == PronunciationAdmission::product_admitted) {
        if (!review.is_string()) {
            throw ValidationError("LTS_RESOURCE_ADMISSION",
                                  "product LTS model lacks a review record hash");
        }
        header.review_record_sha256 = string_value(review);
        if (!is_lower_sha256(header.review_record_sha256)) {
            throw ValidationError("LTS_RESOURCE_ADMISSION",
                                  "product LTS review binding is invalid");
        }
    } else if (!review.is_null()) {
        throw ValidationError("LTS_RESOURCE_ADMISSION",
                              "non-product LTS model must not claim review");
    }

    const json::Value::Object &roots = object_value(required(object, "roots"));
    if (roots.empty() || roots.size() > 28U) {
        throw ValidationError("LTS_RESOURCE_LIMIT",
                              "LTS root count is outside its limit");
    }
    for (const auto &[symbol_text, root_value] : roots) {
        if (symbol_text.size() != 1U || !word_symbol(symbol_text.front())) {
            throw ValidationError("LTS_RESOURCE_SCHEMA",
                                  "LTS root symbol is not canonical");
        }
        const std::size_t root = bounded_count(root_value, 0U,
                                               header.node_count - 1U,
                                               "LTS root node ID");
        header.roots.emplace(symbol_text.front(),
                             static_cast<std::uint32_t>(root));
    }
    if (header.admission == PronunciationAdmission::product_admitted) {
        for (char symbol = 'a'; symbol <= 'z'; ++symbol) {
            if (header.roots.find(symbol) == header.roots.end()) {
                throw ValidationError("LTS_RESOURCE_ADMISSION",
                                      "product LTS model lacks an ASCII letter root");
            }
        }
    }
    return header;
}

ParsedNode parse_node(
    const json::Value &document,
    std::size_t expected_id,
    const Header &header,
    const std::set<std::uint16_t> &segment_ids) {
    const json::Value::Object &object = object_value(document);
    const std::string &kind = string_value(required(object, "kind"));
    if (kind == "decision") {
        if (!exact_keys(object,
                        {"feature_offset", "feature_value", "id", "kind", "no",
                         "schema", "yes"})) {
            throw ValidationError("LTS_RESOURCE_SCHEMA",
                                  "LTS decision node has unknown or absent fields");
        }
    } else if (kind == "leaf") {
        if (!exact_keys(object, {"emissions", "id", "kind", "schema"})) {
            throw ValidationError("LTS_RESOURCE_SCHEMA",
                                  "LTS leaf node has unknown or absent fields");
        }
    } else {
        throw ValidationError("LTS_RESOURCE_SCHEMA", "LTS node kind is unknown");
    }
    if (string_value(required(object, "schema")) !=
        "kilix.voicegen.lts-node/v1") {
        throw ValidationError("LTS_RESOURCE_SCHEMA",
                              "LTS node schema is unsupported");
    }
    const std::size_t id = bounded_count(required(object, "id"), 0U,
                                         header.node_count - 1U,
                                         "LTS node ID");
    if (id != expected_id) {
        throw ValidationError("LTS_RESOURCE_ORDER",
                              "LTS node IDs must be contiguous and ordered");
    }

    ParsedNode node;
    if (kind == "decision") {
        const std::int64_t offset = integer_value(required(object, "feature_offset"),
                                                  "LTS feature offset");
        const std::int64_t left = -static_cast<std::int64_t>(header.context_left);
        const std::int64_t right = static_cast<std::int64_t>(header.context_right);
        if (offset == 0 || offset < left || offset > right ||
            offset < std::numeric_limits<std::int8_t>::min() ||
            offset > std::numeric_limits<std::int8_t>::max()) {
            throw ValidationError("LTS_RESOURCE_SCHEMA",
                                  "LTS feature offset is outside declared context");
        }
        const std::string &feature =
            string_value(required(object, "feature_value"));
        if (feature.size() != 1U || !feature_symbol(feature.front())) {
            throw ValidationError("LTS_RESOURCE_SCHEMA",
                                  "LTS feature value is not canonical");
        }
        node.feature_offset = static_cast<std::int8_t>(offset);
        node.feature_value = feature.front();
        node.yes = static_cast<std::uint32_t>(bounded_count(
            required(object, "yes"), 0U, header.node_count - 1U,
            "LTS yes node ID"));
        node.no = static_cast<std::uint32_t>(bounded_count(
            required(object, "no"), 0U, header.node_count - 1U,
            "LTS no node ID"));
        return node;
    }

    node.leaf = true;
    const json::Value::Array &emissions =
        array_value(required(object, "emissions"));
    if (emissions.size() > kMaximumEmissionsPerLeaf) {
        throw ValidationError("LTS_RESOURCE_LIMIT",
                              "LTS leaf emission count exceeds its limit");
    }
    std::size_t leaf_segment_count = 0U;
    for (const json::Value &emission_value : emissions) {
        const json::Value::Object &emission_object = object_value(emission_value);
        if (!exact_keys(emission_object, {"segment_ids", "syllable_end"})) {
            throw ValidationError("LTS_RESOURCE_SCHEMA",
                                  "LTS emission has unknown or absent fields");
        }
        const json::Value::Array &segments =
            array_value(required(emission_object, "segment_ids"));
        if (segments.empty() || segments.size() > kMaximumSegmentsPerEmission) {
            throw ValidationError("LTS_RESOURCE_LIMIT",
                                  "LTS emission segment count is outside its limit");
        }
        ParsedEmission emission;
        for (const json::Value &segment_value : segments) {
            const std::size_t id_value = bounded_count(
                segment_value, 1U,
                static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()),
                "LTS segment ID");
            const std::uint16_t segment_id = static_cast<std::uint16_t>(id_value);
            if (segment_ids.find(segment_id) == segment_ids.end()) {
                throw ValidationError("LTS_UNKNOWN_SEGMENT",
                                      "LTS leaf uses an ID outside the inventory");
            }
            emission.segment_ids.push_back(segment_id);
        }
        leaf_segment_count += segments.size();
        if (leaf_segment_count > kMaximumSegmentsPerLeaf) {
            throw ValidationError("LTS_RESOURCE_LIMIT",
                                  "LTS leaf segment count exceeds its limit");
        }
        const json::Value &syllable_end =
            required(emission_object, "syllable_end");
        if (!syllable_end.is_null()) {
            emission.ends_syllable = true;
            emission.stress = parse_stress(string_value(syllable_end));
        }
        node.emissions.push_back(std::move(emission));
    }
    return node;
}

void validate_graph(const Header &header, const std::vector<ParsedNode> &nodes) {
    std::vector<std::uint8_t> state(nodes.size(), 0U);
    std::vector<std::size_t> depth(nodes.size(), 0U);
    const std::function<std::size_t(std::uint32_t, std::size_t)> visit =
        [&](std::uint32_t raw_id, std::size_t path_depth) -> std::size_t {
        if (path_depth > header.maximum_steps) {
            throw ValidationError("LTS_RESOURCE_GRAPH",
                                  "LTS path exceeds its declared maximum steps");
        }
        const std::size_t id = raw_id;
        if (id >= nodes.size()) {
            throw ValidationError("LTS_RESOURCE_GRAPH",
                                  "LTS node references an absent node");
        }
        if (state[id] == 1U) {
            throw ValidationError("LTS_RESOURCE_GRAPH",
                                  "LTS graph contains a cycle");
        }
        if (state[id] == 2U) {
            if (path_depth - 1U + depth[id] > header.maximum_steps) {
                throw ValidationError(
                    "LTS_RESOURCE_GRAPH",
                    "LTS path exceeds its declared maximum steps");
            }
            return depth[id];
        }
        state[id] = 1U;
        const ParsedNode &node = nodes[id];
        std::size_t result = 1U;
        if (!node.leaf) {
            result += std::max(visit(node.yes, path_depth + 1U),
                               visit(node.no, path_depth + 1U));
        }
        if (result > header.maximum_steps) {
            throw ValidationError("LTS_RESOURCE_GRAPH",
                                  "LTS path exceeds its declared maximum steps");
        }
        depth[id] = result;
        state[id] = 2U;
        return result;
    };

    for (const auto &[symbol, root] : header.roots) {
        (void)symbol;
        (void)visit(root, 1U);
    }
    if (std::any_of(state.begin(), state.end(),
                    [](std::uint8_t value) { return value != 2U; })) {
        throw ValidationError("LTS_RESOURCE_GRAPH",
                              "LTS graph contains unreachable nodes");
    }
}

char context_symbol(std::string_view word,
                    std::size_t position,
                    std::int8_t offset) noexcept {
    const std::ptrdiff_t target = static_cast<std::ptrdiff_t>(position) +
                                  static_cast<std::ptrdiff_t>(offset);
    if (target < 0) {
        return '^';
    }
    const std::size_t unsigned_target = static_cast<std::size_t>(target);
    return unsigned_target >= word.size() ? '$' : word[unsigned_target];
}

}  // namespace

int LtsModel::pronounce(std::string_view grapheme,
                        std::vector<PronunciationSyllable> *syllables,
                        LtsFailure *failure) const {
    if (syllables == nullptr || failure == nullptr) {
        return reject_pronunciation(syllables, failure, KGV_INVALID_ARGUMENT,
                                    "INVALID_LTS_PRONOUNCE_ARGUMENT",
                                    "LTS pronunciation requires output records");
    }
    syllables->clear();
    *failure = LtsFailure{};
    if (nodes_.empty() || roots_.empty() || maximum_steps_ == 0U) {
        return reject_pronunciation(syllables, failure, KGV_INVALID_STATE,
                                    "LTS_MODEL_NOT_LOADED",
                                    "LTS model is not loaded");
    }
    if (!canonical_word(grapheme)) {
        return reject_pronunciation(syllables, failure, KGV_INVALID_TEXT,
                                    "LTS_INVALID_GRAPHEME",
                                    "LTS input must be canonical lowercase ASCII");
    }

    PronunciationSyllable current;
    std::size_t total_segments = 0U;
    std::size_t primary_stresses = 0U;
    for (std::size_t position = 0U; position < grapheme.size(); ++position) {
        const auto root = roots_.find(grapheme[position]);
        if (root == roots_.end()) {
            return reject_pronunciation(syllables, failure, KGV_INVALID_TEXT,
                                        "LTS_UNSUPPORTED_GRAPHEME",
                                        "LTS model has no root for an input symbol");
        }
        std::uint32_t node_id = root->second;
        const Node *leaf = nullptr;
        for (std::uint32_t step = 0U; step < maximum_steps_; ++step) {
            if (node_id >= nodes_.size()) {
                return reject_pronunciation(syllables, failure, KGV_INVALID_MODEL,
                                            "LTS_RESOURCE_GRAPH",
                                            "LTS graph references an absent node");
            }
            const Node &node = nodes_[node_id];
            if (node.leaf) {
                leaf = &node;
                break;
            }
            const char value =
                context_symbol(grapheme, position, node.feature_offset);
            node_id = value == node.feature_value ? node.yes : node.no;
        }
        if (leaf == nullptr) {
            return reject_pronunciation(syllables, failure, KGV_INVALID_MODEL,
                                        "LTS_RESOURCE_GRAPH",
                                        "LTS inference exceeded maximum steps");
        }
        for (const Emission &emission : leaf->emissions) {
            total_segments += emission.segment_ids.size();
            if (total_segments > kMaximumSegmentsPerWord) {
                return reject_pronunciation(syllables, failure,
                                            KGV_RESOURCE_EXHAUSTED,
                                            "LTS_PRONUNCIATION_LIMIT",
                                            "LTS output exceeds the segment limit");
            }
            current.segment_ids.insert(current.segment_ids.end(),
                                       emission.segment_ids.begin(),
                                       emission.segment_ids.end());
            if (emission.ends_syllable) {
                if (current.segment_ids.empty() ||
                    syllables->size() >= kMaximumSyllablesPerWord) {
                    return reject_pronunciation(syllables, failure,
                                                KGV_INVALID_MODEL,
                                                "LTS_INVALID_EMISSION_SEQUENCE",
                                                "LTS emitted an invalid syllable boundary");
                }
                current.stress = emission.stress;
                if (current.stress == SyllableStress::primary) {
                    ++primary_stresses;
                    if (primary_stresses > 1U) {
                        return reject_pronunciation(
                            syllables, failure, KGV_INVALID_MODEL,
                            "LTS_INVALID_EMISSION_SEQUENCE",
                            "LTS emitted multiple primary stresses");
                    }
                }
                syllables->push_back(std::move(current));
                current = PronunciationSyllable{};
            }
        }
    }
    if (!current.segment_ids.empty() || syllables->empty()) {
        return reject_pronunciation(syllables, failure, KGV_INVALID_MODEL,
                                    "LTS_INVALID_EMISSION_SEQUENCE",
                                    "LTS output is empty or has an open syllable");
    }
    return KGV_OK;
}

const std::string &LtsModel::resource_id() const noexcept {
    return resource_id_;
}

const std::string &LtsModel::resource_sha256() const noexcept {
    return resource_sha256_;
}

const std::string &LtsModel::segment_inventory_sha256() const noexcept {
    return segment_inventory_sha256_;
}

const std::string &LtsModel::source_lexicon_sha256() const noexcept {
    return source_lexicon_sha256_;
}

const std::string &LtsModel::training_record_sha256() const noexcept {
    return training_record_sha256_;
}

const std::string &LtsModel::review_record_sha256() const noexcept {
    return review_record_sha256_;
}

PronunciationAdmission LtsModel::admission() const noexcept {
    return admission_;
}

std::size_t LtsModel::node_count() const noexcept {
    return nodes_.size();
}

std::size_t LtsModel::root_count() const noexcept {
    return roots_.size();
}

int load_lts_model(
    std::string_view jsonl,
    std::string_view expected_resource_sha256,
    std::string_view expected_segment_inventory_sha256,
    PronunciationAdmission required_admission,
    const std::vector<SegmentDefinition> &segments,
    LtsModel *model,
    LtsFailure *failure) {
    if (model == nullptr || failure == nullptr) {
        return reject(model, failure, KGV_INVALID_ARGUMENT,
                      "INVALID_LTS_LOADER_ARGUMENT",
                      "LTS loader requires output records", 0U);
    }
    *model = LtsModel{};
    *failure = LtsFailure{};
    if (!is_lower_sha256(expected_resource_sha256) ||
        !is_lower_sha256(expected_segment_inventory_sha256) ||
        !valid_admission(required_admission) || segments.empty()) {
        return reject(model, failure, KGV_INVALID_ARGUMENT,
                      "INVALID_LTS_LOADER_ARGUMENT",
                      "LTS loader requires pinned hashes, admission, and segments",
                      0U);
    }
    const std::string actual_inventory_sha256 =
        pronunciation_segment_inventory_sha256(segments);
    if (actual_inventory_sha256.empty()) {
        return reject(model, failure, KGV_INVALID_ARGUMENT,
                      "INVALID_LTS_SEGMENT_INVENTORY",
                      "LTS segment inventory is not canonical", 0U);
    }
    if (actual_inventory_sha256 != expected_segment_inventory_sha256) {
        return reject(model, failure, KGV_ABI_MISMATCH,
                      "LTS_SEGMENT_INVENTORY_HASH_MISMATCH",
                      "LTS segment definitions do not match the pinned inventory",
                      0U);
    }
    if (jsonl.empty() || jsonl.size() > kMaximumResourceBytes) {
        return reject(model, failure, KGV_RESOURCE_EXHAUSTED,
                      "LTS_RESOURCE_TOO_LARGE",
                      "LTS resource is empty or exceeds 64 MiB", 0U);
    }
    if (sha256_hex(jsonl) != expected_resource_sha256) {
        return reject(model, failure, KGV_HASH_MISMATCH,
                      "LTS_RESOURCE_HASH_MISMATCH",
                      "LTS resource does not match its pinned SHA-256", 0U);
    }
    if (jsonl.back() != '\n') {
        return reject(model, failure, KGV_INVALID_MODEL,
                      "LTS_RESOURCE_CANONICAL_FORM",
                      "LTS JSONL must end with LF", 0U);
    }

    std::set<std::uint16_t> segment_ids;
    for (const SegmentDefinition &segment : segments) {
        segment_ids.insert(segment.id);
    }
    Header header;
    bool saw_header = false;
    std::vector<ParsedNode> nodes;
    std::size_t cursor = 0U;
    std::size_t line_number = 1U;
    while (cursor < jsonl.size()) {
        const std::size_t end = jsonl.find('\n', cursor);
        if (end == std::string_view::npos) {
            return reject(model, failure, KGV_INVALID_MODEL,
                          "LTS_RESOURCE_CANONICAL_FORM",
                          "LTS JSONL contains an unterminated line", line_number);
        }
        const std::string_view line = jsonl.substr(cursor, end - cursor);
        if (line.empty() || line.size() > kMaximumLineBytes ||
            line.find('\r') != std::string_view::npos) {
            return reject(model, failure, KGV_INVALID_MODEL,
                          "LTS_RESOURCE_CANONICAL_FORM",
                          "LTS JSONL has an empty, oversized, or CR line",
                          line_number);
        }
        json::Value document;
        try {
            document = json::parse(line);
        } catch (const json::ParseError &) {
            return reject(model, failure, KGV_INVALID_MODEL,
                          "LTS_RESOURCE_JSON",
                          "LTS resource line is not strict JSON", line_number);
        }
        try {
            if (!saw_header) {
                header = parse_header(document, required_admission,
                                      expected_segment_inventory_sha256);
                saw_header = true;
                nodes.reserve(header.node_count);
            } else {
                if (nodes.size() >= header.node_count) {
                    throw ValidationError("LTS_RESOURCE_NODE_COUNT",
                                          "LTS resource has extra nodes");
                }
                nodes.push_back(parse_node(document, nodes.size(), header,
                                           segment_ids));
            }
        } catch (const ValidationError &error) {
            const int status = error.code() == "LTS_RESOURCE_INVENTORY_MISMATCH"
                                   ? KGV_ABI_MISMATCH
                                   : KGV_INVALID_MODEL;
            return reject(model, failure, status, error.code(), error.what(),
                          line_number);
        } catch (const std::exception &) {
            return reject(model, failure, KGV_INVALID_MODEL,
                          "LTS_RESOURCE_SCHEMA",
                          "LTS resource line has an invalid value type",
                          line_number);
        }
        cursor = end + 1U;
        ++line_number;
    }
    if (!saw_header || nodes.size() != header.node_count) {
        return reject(model, failure, KGV_INVALID_MODEL,
                      "LTS_RESOURCE_NODE_COUNT",
                      "LTS node count does not match the header", 1U);
    }
    try {
        validate_graph(header, nodes);
    } catch (const ValidationError &error) {
        return reject(model, failure, KGV_INVALID_MODEL, error.code(),
                      error.what(), 1U);
    }

    LtsModel candidate;
    candidate.resource_id_ = std::move(header.resource_id);
    candidate.resource_sha256_ = std::string(expected_resource_sha256);
    candidate.segment_inventory_sha256_ =
        std::move(header.segment_inventory_sha256);
    candidate.source_lexicon_sha256_ =
        std::move(header.source_lexicon_sha256);
    candidate.training_record_sha256_ =
        std::move(header.training_record_sha256);
    candidate.review_record_sha256_ =
        std::move(header.review_record_sha256);
    candidate.admission_ = header.admission;
    candidate.maximum_steps_ = header.maximum_steps;
    candidate.roots_ = std::move(header.roots);
    candidate.nodes_.reserve(nodes.size());
    for (ParsedNode &parsed : nodes) {
        LtsModel::Node node;
        node.leaf = parsed.leaf;
        node.feature_offset = parsed.feature_offset;
        node.feature_value = parsed.feature_value;
        node.yes = parsed.yes;
        node.no = parsed.no;
        node.emissions.reserve(parsed.emissions.size());
        for (ParsedEmission &parsed_emission : parsed.emissions) {
            LtsModel::Emission emission;
            emission.segment_ids = std::move(parsed_emission.segment_ids);
            emission.ends_syllable = parsed_emission.ends_syllable;
            emission.stress = parsed_emission.stress;
            node.emissions.push_back(std::move(emission));
        }
        candidate.nodes_.push_back(std::move(node));
    }
    *model = std::move(candidate);
    return KGV_OK;
}

}  // namespace kgv
