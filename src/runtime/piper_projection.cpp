#include "runtime/piper_projection.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kilix_voicegen.h"
#include "runtime/json.h"
#include "runtime/sha256.h"

namespace kgv {
namespace {

constexpr std::size_t kMaximumResourceBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumLineBytes = 64U * 1024U;
constexpr std::size_t kMaximumTargetsPerEntry = 8U;
constexpr std::size_t kMaximumTotalProjectedTokens = 1048576U;

class ValidationError final : public std::runtime_error {
public:
    explicit ValidationError(const std::string &message)
        : std::runtime_error(message) {}
};

bool exact_keys(const json::Value::Object &object,
                std::initializer_list<std::string_view> keys) {
    if (object.size() != keys.size()) {
        return false;
    }
    return std::all_of(keys.begin(), keys.end(), [&object](std::string_view key) {
        return object.find(key) != object.end();
    });
}

const json::Value &required(const json::Value::Object &object,
                            std::string_view key) {
    const auto found = object.find(key);
    if (found == object.end()) {
        throw ValidationError("token projection is missing a required field");
    }
    return found->second;
}

const json::Value::Object &object_value(const json::Value &value) {
    if (!value.is_object()) {
        throw ValidationError("token projection field has the wrong type");
    }
    return value.as_object();
}

const json::Value::Array &array_value(const json::Value &value) {
    if (!value.is_array()) {
        throw ValidationError("token projection field has the wrong type");
    }
    return value.as_array();
}

const std::string &string_value(const json::Value &value,
                                std::size_t maximum = 128U) {
    if (!value.is_string()) {
        throw ValidationError("token projection field has the wrong type");
    }
    const std::string &result = value.as_string();
    if (result.empty() || result.size() > maximum) {
        throw ValidationError("token projection string length is invalid");
    }
    return result;
}

std::uint64_t unsigned_value(const json::Value &value) {
    if (!value.is_number()) {
        throw ValidationError("token projection field has the wrong type");
    }
    std::int64_t parsed = 0;
    try {
        parsed = value.as_integer();
    } catch (const std::exception &) {
        throw ValidationError("token projection integer is invalid");
    }
    if (parsed < 0) {
        throw ValidationError("token projection integer must be nonnegative");
    }
    return static_cast<std::uint64_t>(parsed);
}

bool stable_resource_id(std::string_view value) noexcept {
    if (value.empty() || value.size() > 128U || value.front() < 'a' ||
        value.front() > 'z') {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '.' ||
               character == '_' || character == '-';
    });
}

int reject(PiperTokenProjection *projection,
           std::string *error,
           int status,
           std::string message) {
    if (projection != nullptr) {
        *projection = PiperTokenProjection{};
    }
    if (error != nullptr) {
        *error = std::move(message);
    }
    return status;
}

int reject_chunks(std::vector<PiperProjectedChunk> *chunks,
                  std::string *error,
                  int status,
                  std::string message) {
    if (chunks != nullptr) {
        chunks->clear();
    }
    if (error != nullptr) {
        *error = std::move(message);
    }
    return status;
}

std::vector<std::int64_t> parse_targets(const json::Value &value,
                                        std::uint16_t target_id_max) {
    const auto &array = array_value(value);
    if (array.size() > kMaximumTargetsPerEntry) {
        throw ValidationError("token projection entry has too many target IDs");
    }
    std::vector<std::int64_t> result;
    result.reserve(array.size());
    for (const json::Value &entry : array) {
        const std::uint64_t target = unsigned_value(entry);
        if (target > target_id_max) {
            throw ValidationError("token projection target ID exceeds the graph inventory");
        }
        result.push_back(static_cast<std::int64_t>(target));
    }
    return result;
}

void require_control_projection(const ModelTokenInventory &inventory,
                                const std::map<std::uint16_t,
                                               std::vector<std::int64_t>> &entries,
                                std::string_view name,
                                std::initializer_list<std::int64_t> expected) {
    const auto source_id = inventory.control_id(name);
    if (!source_id) {
        throw ValidationError("source token inventory lacks a required control");
    }
    const auto found = entries.find(*source_id);
    if (found == entries.end() ||
        !std::equal(found->second.begin(), found->second.end(),
                    expected.begin(), expected.end())) {
        throw ValidationError("token projection has an invalid Piper control mapping");
    }
}

}  // namespace

const std::string &PiperTokenProjection::resource_id() const noexcept {
    return resource_id_;
}

const std::string &PiperTokenProjection::resource_sha256() const noexcept {
    return resource_sha256_;
}

const std::string &PiperTokenProjection::source_token_inventory_sha256() const noexcept {
    return source_token_inventory_sha256_;
}

std::size_t PiperTokenProjection::maximum_output_tokens() const noexcept {
    return maximum_output_tokens_;
}

std::uint16_t PiperTokenProjection::target_id_max() const noexcept {
    return target_id_max_;
}

int load_piper_token_projection(
    std::string_view jsonl,
    std::string_view expected_resource_sha256,
    const ModelTokenInventory &source_inventory,
    std::uint16_t expected_target_id_max,
    PiperTokenProjection *projection,
    std::string *error) {
    if (projection == nullptr || error == nullptr) {
        return KGV_INVALID_ARGUMENT;
    }
    *projection = PiperTokenProjection{};
    error->clear();
    if (!is_lower_sha256(expected_resource_sha256) ||
        !is_lower_sha256(source_inventory.resource_sha256()) ||
        source_inventory.entry_count() == 0U || expected_target_id_max != 255U) {
        return reject(projection, error, KGV_INVALID_ARGUMENT,
                      "Piper token projection loader requires pinned v1 inputs");
    }
    if (jsonl.empty() || jsonl.size() > kMaximumResourceBytes ||
        jsonl.back() != '\n' || jsonl.find('\r') != std::string_view::npos ||
        jsonl.find('\0') != std::string_view::npos) {
        return reject(projection, error, KGV_INVALID_MODEL,
                      "Piper token projection is empty, oversized, or non-canonical");
    }
    if (sha256_hex(jsonl) != expected_resource_sha256) {
        return reject(projection, error, KGV_HASH_MISMATCH,
                      "Piper token projection does not match its pinned SHA-256");
    }

    try {
        std::size_t cursor = 0U;
        bool saw_header = false;
        std::string resource_id;
        std::string source_sha;
        std::size_t entry_count = 0U;
        std::size_t maximum_output_tokens = 0U;
        std::uint16_t target_id_max = 0U;
        std::map<std::uint16_t, std::vector<std::int64_t>> entries;
        std::uint16_t previous_source_id = 0U;
        bool saw_entry = false;

        while (cursor < jsonl.size()) {
            const std::size_t end = jsonl.find('\n', cursor);
            if (end == std::string_view::npos || end == cursor ||
                end - cursor > kMaximumLineBytes) {
                throw ValidationError("token projection has an empty, oversized, or unterminated line");
            }
            const std::string_view line = jsonl.substr(cursor, end - cursor);
            json::Value document;
            try {
                document = json::parse(line);
            } catch (const json::ParseError &) {
                throw ValidationError("token projection line is not strict JSON");
            }
            const auto &object = object_value(document);
            if (!saw_header) {
                if (!exact_keys(object, {"schema", "resource_id", "engine_kind",
                                         "source_token_inventory_sha256",
                                         "entry_count", "maximum_output_tokens",
                                         "target_id_max"}) ||
                    string_value(required(object, "schema")) !=
                        "kilix.voicegen.token-projection/v1" ||
                    string_value(required(object, "engine_kind")) !=
                        "piper-vits-onnx/v1") {
                    throw ValidationError("token projection header schema is unsupported");
                }
                resource_id = string_value(required(object, "resource_id"));
                if (!stable_resource_id(resource_id)) {
                    throw ValidationError("token projection resource ID is invalid");
                }
                source_sha = string_value(
                    required(object, "source_token_inventory_sha256"), 64U);
                if (source_sha != source_inventory.resource_sha256()) {
                    return reject(projection, error, KGV_ABI_MISMATCH,
                                  "token projection source inventory binding does not match");
                }
                const std::uint64_t raw_entry_count =
                    unsigned_value(required(object, "entry_count"));
                const std::uint64_t raw_maximum =
                    unsigned_value(required(object, "maximum_output_tokens"));
                const std::uint64_t raw_target_max =
                    unsigned_value(required(object, "target_id_max"));
                if (raw_entry_count != source_inventory.entry_count() ||
                    raw_maximum < 8U || raw_maximum > 8192U ||
                    raw_target_max != expected_target_id_max) {
                    throw ValidationError("token projection header limits are incompatible");
                }
                entry_count = static_cast<std::size_t>(raw_entry_count);
                maximum_output_tokens = static_cast<std::size_t>(raw_maximum);
                target_id_max = static_cast<std::uint16_t>(raw_target_max);
                saw_header = true;
            } else {
                if (!exact_keys(object, {"schema", "source_id", "target_ids"}) ||
                    string_value(required(object, "schema")) !=
                        "kilix.voicegen.token-projection-entry/v1") {
                    throw ValidationError("token projection entry schema is unsupported");
                }
                const std::uint64_t raw_source_id =
                    unsigned_value(required(object, "source_id"));
                if (raw_source_id > std::numeric_limits<std::uint16_t>::max()) {
                    throw ValidationError("token projection source ID is outside uint16");
                }
                const auto source_id = static_cast<std::uint16_t>(raw_source_id);
                if ((saw_entry && source_id <= previous_source_id) ||
                    entries.size() >= entry_count) {
                    throw ValidationError("token projection source IDs are duplicated or unordered");
                }
                entries.emplace(source_id,
                                parse_targets(required(object, "target_ids"),
                                              target_id_max));
                previous_source_id = source_id;
                saw_entry = true;
            }
            cursor = end + 1U;
        }
        if (!saw_header || entries.size() != entry_count) {
            throw ValidationError("token projection entry count does not match its header");
        }
        const std::vector<std::uint16_t> expected_ids = source_inventory.token_ids();
        std::vector<std::uint16_t> actual_ids;
        actual_ids.reserve(entries.size());
        for (const auto &entry : entries) {
            actual_ids.push_back(entry.first);
        }
        if (actual_ids != expected_ids) {
            return reject(projection, error, KGV_ABI_MISMATCH,
                          "token projection does not cover the exact source inventory");
        }

        require_control_projection(source_inventory, entries, "PAD", {});
        require_control_projection(source_inventory, entries, "BOS", {1, 0});
        require_control_projection(source_inventory, entries, "EOS", {2});
        require_control_projection(source_inventory, entries, "WB", {3, 0});
        require_control_projection(source_inventory, entries, "SYL", {});
        require_control_projection(source_inventory, entries, "STRESS_0", {});
        require_control_projection(source_inventory, entries, "STRESS_1", {});
        require_control_projection(source_inventory, entries, "STRESS_2", {});
        require_control_projection(source_inventory, entries, "END_NONE", {});
        require_control_projection(source_inventory, entries, "END_COMMA", {8, 0});
        require_control_projection(source_inventory, entries, "END_COLON", {11, 0});
        require_control_projection(source_inventory, entries, "END_SEMICOLON", {12, 0});
        require_control_projection(source_inventory, entries, "END_PERIOD", {10, 0});
        require_control_projection(source_inventory, entries, "END_QUESTION", {13, 0});
        require_control_projection(source_inventory, entries, "END_EXCLAMATION", {4, 0});
        require_control_projection(source_inventory, entries, "END_PARAGRAPH", {10, 0});
        require_control_projection(source_inventory, entries, "END_CONTINUATION", {8, 0});

        std::set<std::uint16_t> control_ids;
        static constexpr std::string_view kControls[] = {
            "PAD", "BOS", "EOS", "WB", "SYL", "STRESS_0", "STRESS_1",
            "STRESS_2", "END_NONE", "END_COMMA", "END_COLON",
            "END_SEMICOLON", "END_PERIOD", "END_QUESTION",
            "END_EXCLAMATION", "END_PARAGRAPH", "END_CONTINUATION",
        };
        for (std::string_view name : kControls) {
            control_ids.insert(*source_inventory.control_id(name));
        }
        for (const auto &entry : entries) {
            if (control_ids.find(entry.first) == control_ids.end() &&
                (entry.second.size() != 2U || entry.second[0] < 4 ||
                 entry.second[1] != 0)) {
                throw ValidationError(
                    "Piper segment projection must contain one phoneme ID and PAD");
            }
        }

        PiperTokenProjection candidate;
        candidate.resource_id_ = std::move(resource_id);
        candidate.resource_sha256_ = std::string(expected_resource_sha256);
        candidate.source_token_inventory_sha256_ = std::move(source_sha);
        candidate.maximum_output_tokens_ = maximum_output_tokens;
        candidate.target_id_max_ = target_id_max;
        candidate.entries_ = std::move(entries);
        *projection = std::move(candidate);
        return KGV_OK;
    } catch (const ValidationError &caught) {
        return reject(projection, error, KGV_INVALID_MODEL, caught.what());
    } catch (const std::bad_alloc &) {
        return reject(projection, error, KGV_RESOURCE_EXHAUSTED,
                      "Piper token projection loading ran out of memory");
    } catch (const std::exception &) {
        return reject(projection, error, KGV_INVALID_MODEL,
                      "Piper token projection contains an invalid value");
    }
}

int project_piper_tokens(
    const PiperTokenProjection &projection,
    const ModelTokenResult &source,
    std::vector<PiperProjectedChunk> *chunks,
    std::string *error) {
    if (chunks == nullptr || error == nullptr) {
        return KGV_INVALID_ARGUMENT;
    }
    chunks->clear();
    error->clear();
    if (projection.entries_.empty() || projection.maximum_output_tokens_ < 8U ||
        source.inventory_sha256 != projection.source_token_inventory_sha256_) {
        return reject_chunks(chunks, error, KGV_ABI_MISMATCH,
                             "Piper projection and resolved token inventory do not match");
    }

    try {
        std::size_t total = 0U;
        chunks->reserve(source.chunks.size());
        for (const ModelTokenChunk &source_chunk : source.chunks) {
            PiperProjectedChunk projected;
            projected.source_span = source_chunk.source_span;
            projected.continuation = source_chunk.continuation;
            for (std::uint16_t source_id : source_chunk.ids) {
                const auto found = projection.entries_.find(source_id);
                if (found == projection.entries_.end()) {
                    return reject_chunks(chunks, error, KGV_ABI_MISMATCH,
                                         "resolved token has no Piper projection");
                }
                if (projected.ids.size() > projection.maximum_output_tokens_ -
                                               found->second.size()) {
                    return reject_chunks(chunks, error, KGV_RESOURCE_EXHAUSTED,
                                         "Piper projected chunk exceeds its model limit");
                }
                projected.ids.insert(projected.ids.end(), found->second.begin(),
                                     found->second.end());
            }
            if (projected.ids.size() < 3U || projected.ids[0] != 1 ||
                projected.ids[1] != 0 || projected.ids.back() != 2) {
                return reject_chunks(chunks, error, KGV_INVALID_MODEL,
                                     "Piper projected chunk lacks canonical BOS/PAD/EOS");
            }
            if (total > kMaximumTotalProjectedTokens - projected.ids.size()) {
                return reject_chunks(chunks, error, KGV_RESOURCE_EXHAUSTED,
                                     "Piper projected output exceeds its global limit");
            }
            total += projected.ids.size();
            chunks->push_back(std::move(projected));
        }
        return KGV_OK;
    } catch (const std::bad_alloc &) {
        return reject_chunks(chunks, error, KGV_RESOURCE_EXHAUSTED,
                             "Piper token projection ran out of memory");
    } catch (const std::exception &) {
        return reject_chunks(chunks, error, KGV_INTERNAL_ERROR,
                             "unexpected Piper token projection failure");
    }
}

}  // namespace kgv
