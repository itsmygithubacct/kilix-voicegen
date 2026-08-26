#include "runtime/model_package.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "runtime/json.h"
#include "runtime/sha256.h"

namespace kgv {
namespace {

constexpr std::uint64_t kMaximumReleaseBytes = 65536U;
constexpr std::uint64_t kMaximumManifestBytes = 1048576U;
constexpr std::size_t kMaximumPayloadFiles = 256U;
constexpr std::uint64_t kMaximumPayloadBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumTotalPayloadBytes =
    4ULL * 1024ULL * 1024ULL * 1024ULL;

class ValidationError final : public std::runtime_error {
public:
    ValidationError(int status, const std::string &message)
        : std::runtime_error(message), status_(status) {}
    int status() const noexcept { return status_; }

private:
    int status_;
};

[[noreturn]] void invalid(const std::string &message) {
    throw ValidationError(KGV_INVALID_MODEL, message);
}

[[noreturn]] void unsupported_schema(const std::string &message) {
    throw ValidationError(KGV_UNSUPPORTED_SCHEMA, message);
}

[[noreturn]] void mismatch(const std::string &message) {
    throw ValidationError(KGV_HASH_MISMATCH, message);
}

[[noreturn]] void abi_mismatch(const std::string &message) {
    throw ValidationError(KGV_ABI_MISMATCH, message);
}

[[noreturn]] void unsupported_cpu(const std::string &message) {
    throw ValidationError(KGV_UNSUPPORTED_CPU, message);
}

[[noreturn]] void io_error(const std::string &message) {
    throw ValidationError(KGV_IO_ERROR, message);
}

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
        invalid("model metadata is missing a required field");
    }
    return found->second;
}

const json::Value::Object &object_value(const json::Value &value) {
    if (!value.is_object()) {
        invalid("model metadata field has the wrong type");
    }
    return value.as_object();
}

const json::Value::Array &array_value(const json::Value &value) {
    if (!value.is_array()) {
        invalid("model metadata field has the wrong type");
    }
    return value.as_array();
}

const std::string &string_value(const json::Value &value,
                                std::size_t maximum = 512U) {
    if (!value.is_string()) {
        invalid("model metadata field has the wrong type");
    }
    const std::string &result = value.as_string();
    if (result.empty() || result.size() > maximum) {
        invalid("model metadata string length is invalid");
    }
    return result;
}

std::uint64_t unsigned_value(const json::Value &value) {
    if (!value.is_number()) {
        invalid("model metadata field has the wrong type");
    }
    std::int64_t number = 0;
    try {
        number = value.as_integer();
    } catch (const std::exception &) {
        invalid("model metadata integer is invalid");
    }
    if (number < 0) {
        invalid("model metadata integer must not be negative");
    }
    return static_cast<std::uint64_t>(number);
}

void require_sha(const std::string &value) {
    if (!is_lower_sha256(value)) {
        invalid("model metadata contains an invalid SHA-256 value");
    }
}

bool safe_relative_path(std::string_view value) {
    if (value.empty() || value.size() > 255U || value.front() == '/' ||
        value.back() == '/' || value.find('\\') != std::string_view::npos ||
        value.find("//") != std::string_view::npos) {
        return false;
    }
    std::size_t component_start = 0U;
    for (std::size_t i = 0U; i <= value.size(); ++i) {
        if (i == value.size() || value[i] == '/') {
            const std::string_view component = value.substr(component_start, i - component_start);
            if (component.empty() || component == "." || component == "..") {
                return false;
            }
            component_start = i + 1U;
            continue;
        }
        const auto byte = static_cast<unsigned char>(value[i]);
        if (!(std::isalnum(byte) != 0 || value[i] == '.' || value[i] == '_' ||
              value[i] == '-')) {
            return false;
        }
    }
    return true;
}

void require_regular_without_symlinks(const std::filesystem::path &root,
                                      std::string_view relative) {
    std::filesystem::path current = root;
    std::error_code code;
    for (const auto &part : std::filesystem::path(std::string(relative))) {
        current /= part;
        const std::filesystem::file_status status = std::filesystem::symlink_status(current, code);
        if (code) {
            io_error("could not inspect a required model file");
        }
        if (std::filesystem::is_symlink(status)) {
            invalid("model package paths must not traverse symbolic links");
        }
    }
    const std::filesystem::file_status status = std::filesystem::status(current, code);
    if (code || !std::filesystem::is_regular_file(status)) {
        io_error("a required model file is absent or is not a regular file");
    }
}

std::string read_bounded(const std::filesystem::path &path,
                         std::uint64_t maximum,
                         std::uint64_t *size) {
    std::error_code code;
    const std::uintmax_t file_size = std::filesystem::file_size(path, code);
    if (code) {
        io_error("could not determine required model metadata size");
    }
    if (file_size > maximum || file_size > std::numeric_limits<std::size_t>::max()) {
        invalid("required model metadata exceeds its size limit");
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        io_error("could not open required model metadata");
    }
    std::string result(static_cast<std::size_t>(file_size), '\0');
    if (!result.empty()) {
        stream.read(result.data(), static_cast<std::streamsize>(result.size()));
        if (stream.gcount() != static_cast<std::streamsize>(result.size())) {
            io_error("could not read complete model metadata");
        }
    }
    char extra = '\0';
    if (stream.get(extra)) {
        io_error("model metadata changed while it was being read");
    }
    *size = static_cast<std::uint64_t>(result.size());
    return result;
}

std::string read_payload_exact(const std::filesystem::path &path,
                               std::uint64_t declared_bytes) {
    if (declared_bytes > kMaximumPayloadBytes ||
        declared_bytes > std::numeric_limits<std::size_t>::max()) {
        invalid("model payload exceeds the per-file size limit");
    }
    std::error_code code;
    const std::uintmax_t file_size = std::filesystem::file_size(path, code);
    if (code) {
        io_error("could not determine a required model payload size");
    }
    if (file_size != declared_bytes) {
        mismatch("model payload byte count does not match manifest");
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        io_error("could not open a required model payload");
    }
    std::string result(static_cast<std::size_t>(declared_bytes), '\0');
    if (!result.empty()) {
        stream.read(result.data(), static_cast<std::streamsize>(result.size()));
        if (stream.gcount() != static_cast<std::streamsize>(result.size())) {
            io_error("could not read a complete model payload");
        }
    }
    char extra = '\0';
    if (stream.get(extra)) {
        mismatch("model payload changed while it was being read");
    }
    return result;
}

json::Value parse_metadata(std::string_view bytes) {
    try {
        return json::parse(bytes);
    } catch (const json::ParseError &) {
        invalid("required model metadata is not strict JSON");
    }
}

void verify_release(const json::Value &release,
                    std::uint64_t *manifest_bytes,
                    std::string *manifest_sha) {
    const auto &root = object_value(release);
    if (!exact_keys(root, {"schema", "manifest"})) {
        invalid("release index fields do not match schema v1");
    }
    const std::string &schema = string_value(required(root, "schema"));
    if (schema != "kilix.voicegen.release/v1") {
        unsupported_schema("release index schema is not supported");
    }
    const auto &manifest = object_value(required(root, "manifest"));
    if (!exact_keys(manifest, {"path", "bytes", "sha256"})) {
        invalid("release manifest reference fields do not match schema v1");
    }
    if (string_value(required(manifest, "path")) != "MANIFEST.json") {
        invalid("release index must select MANIFEST.json");
    }
    *manifest_bytes = unsigned_value(required(manifest, "bytes"));
    if (*manifest_bytes == 0U || *manifest_bytes > kMaximumManifestBytes) {
        invalid("declared model manifest size is invalid");
    }
    *manifest_sha = string_value(required(manifest, "sha256"));
    require_sha(*manifest_sha);
}

void verify_revisions(const json::Value &value) {
    const auto &object = object_value(value);
    if (!exact_keys(object, {"architecture", "export", "training"})) {
        invalid("model revision fields do not match schema v1");
    }
    (void)string_value(required(object, "architecture"), 128U);
    (void)string_value(required(object, "export"), 128U);
    (void)string_value(required(object, "training"), 128U);
}

void verify_runtime_abi(const json::Value &value) {
    const auto &object = object_value(value);
    if (!exact_keys(object, {"minimum", "maximum"})) {
        invalid("runtime ABI fields do not match schema v1");
    }
    const std::uint64_t minimum = unsigned_value(required(object, "minimum"));
    const std::uint64_t maximum = unsigned_value(required(object, "maximum"));
    if (minimum == 0U || maximum < minimum) {
        invalid("model runtime ABI range is invalid");
    }
    if (minimum > KILIX_VOICEGEN_ABI_VERSION || maximum < KILIX_VOICEGEN_ABI_VERSION) {
        abi_mismatch("model package is incompatible with runtime ABI 1");
    }
}

void verify_audio(const json::Value &value, VerifiedModel *model) {
    const auto &object = object_value(value);
    if (!exact_keys(object, {"sample_rate", "channels", "sample_format"})) {
        invalid("audio metadata fields do not match schema v1");
    }
    const std::uint64_t sample_rate = unsigned_value(required(object, "sample_rate"));
    if (sample_rate != KGV_SAMPLE_RATE || unsigned_value(required(object, "channels")) != 1U ||
        string_value(required(object, "sample_format")) != "s16") {
        invalid("model audio format must be 24 kHz mono signed-16 PCM");
    }
    model->sample_rate = static_cast<std::uint32_t>(sample_rate);
}

void verify_frontend(const json::Value &value, VerifiedModel *model) {
    const auto &object = object_value(value);
    if (!exact_keys(object, {"schema", "dialect", "unicode_version", "token_schema",
                             "inventory_sha256", "segment_ids", "admission",
                             "abi_sha256"})) {
        invalid("frontend metadata fields do not match schema v1");
    }
    if (string_value(required(object, "schema")) != "kilix.voicegen.frontend/v1" ||
        string_value(required(object, "token_schema")) != "kilix.voicegen.tokens/v1") {
        unsupported_schema("model frontend or token schema is not supported");
    }
    if (string_value(required(object, "dialect")) != "en-AU") {
        unsupported_schema("model frontend dialect is not supported");
    }
    if (string_value(required(object, "unicode_version")) != "17.0.0") {
        unsupported_schema("model Unicode version is not supported");
    }
    model->segment_inventory_sha256 =
        string_value(required(object, "inventory_sha256"));
    require_sha(model->segment_inventory_sha256);
    model->frontend_abi_sha256 = string_value(required(object, "abi_sha256"));
    require_sha(model->frontend_abi_sha256);
    model->frontend_admission = string_value(required(object, "admission"), 32U);
    if (model->frontend_admission != "product-admitted" &&
        model->frontend_admission != "test-fixture") {
        invalid("model frontend admission is unsupported");
    }
    const auto &segments = array_value(required(object, "segment_ids"));
    if (segments.empty() || segments.size() > 65535U) {
        invalid("frontend segment inventory size is invalid");
    }
    std::uint64_t previous = 0U;
    for (const auto &entry : segments) {
        const std::uint64_t id = unsigned_value(entry);
        if (id == 0U || id > 65535U || id <= previous) {
            invalid("frontend segment IDs must be unique ascending uint16 values");
        }
        model->segment_ids.push_back(static_cast<std::uint16_t>(id));
        previous = id;
    }
}

void verify_voices(const json::Value &value, VerifiedModel *model) {
    const auto &voices = array_value(value);
    if (voices.empty() || voices.size() > 2U) {
        invalid("model manifest must declare one or two v1 voices");
    }
    std::set<std::string> found;
    for (std::size_t i = 0U; i < voices.size(); ++i) {
        const auto &voice = object_value(voices[i]);
        if (!exact_keys(voice, {"id", "label"})) {
            invalid("voice metadata fields do not match schema v1");
        }
        const std::string &id = string_value(required(voice, "id"), 64U);
        if (id != "kilix-female-01" && id != "kilix-male-01") {
            invalid("model contains an unknown v1 voice ID");
        }
        if (!found.insert(id).second) {
            invalid("model contains a duplicate voice ID");
        }
        (void)string_value(required(voice, "label"), 80U);
        model->voice_ids.push_back(id);
    }
}

void verify_quantization(const json::Value &value, std::string_view engine_kind) {
    const auto &object = object_value(value);
    if (!exact_keys(object, {"precision", "policy"})) {
        invalid("quantization fields do not match schema v1");
    }
    const std::string &precision = string_value(required(object, "precision"), 32U);
    (void)string_value(required(object, "policy"), 256U);
    if (engine_kind == "fixture-tone/v1" && precision != "fixture") {
        invalid("fixture engine requires fixture precision metadata");
    }
}

void verify_cpu_features(const json::Value &value) {
    const auto &features = array_value(value);
    if (features.size() > 32U) {
        invalid("too many required CPU features are declared");
    }
    std::set<std::string> seen;
    for (const auto &entry : features) {
        const std::string &feature = string_value(entry, 64U);
        if (!seen.insert(feature).second) {
            invalid("duplicate required CPU feature");
        }
        if (feature == "sse2") {
#if !defined(__x86_64__) && !defined(_M_X64)
            unsupported_cpu("model requires SSE2 on a non-x86-64 runtime");
#endif
            continue;
        }
        unsupported_cpu("model declares an unsupported CPU feature");
    }
}

void verify_files(const json::Value &value,
                  const std::filesystem::path &root,
                  std::uint64_t expected_model_bytes,
                  std::string_view engine_kind,
                  VerifiedModel *model,
                  std::set<std::string> *verified_paths) {
    const auto &files = array_value(value);
    if (files.empty() || files.size() > kMaximumPayloadFiles) {
        invalid("model payload file count is invalid");
    }
    std::set<std::string> paths;
    std::set<std::string> roles;
    std::uint64_t total_bytes = 0U;
    std::vector<VerifiedPayload> payloads;
    payloads.reserve(files.size());
    for (const auto &entry : files) {
        const auto &file = object_value(entry);
        if (!exact_keys(file, {"path", "role", "bytes", "sha256"})) {
            invalid("payload file fields do not match schema v1");
        }
        const std::string &path = string_value(required(file, "path"), 255U);
        const std::string &role = string_value(required(file, "role"), 64U);
        const std::uint64_t declared_bytes = unsigned_value(required(file, "bytes"));
        const std::string &declared_sha = string_value(required(file, "sha256"));
        require_sha(declared_sha);
        if (!safe_relative_path(path) || path == "RELEASE.json" || path == "MANIFEST.json" ||
            path == "RELEASE.sha256") {
            invalid("payload path is not a safe relative model path");
        }
        if (!paths.insert(path).second) {
            invalid("model manifest contains a duplicate payload path");
        }
        if (!roles.insert(role).second) {
            invalid("model manifest contains a duplicate payload role");
        }
        require_regular_without_symlinks(root, path);
        std::string payload = read_payload_exact(root / path, declared_bytes);
        if (sha256_hex(payload) != declared_sha) {
            mismatch("model payload byte count or SHA-256 does not match manifest");
        }
        if (total_bytes > kMaximumTotalPayloadBytes - declared_bytes) {
            invalid("model payload size total overflowed");
        }
        total_bytes += declared_bytes;
        payloads.push_back(VerifiedPayload{path, role, declared_sha,
                                           std::move(payload)});
    }
    if (engine_kind == "fixture-tone/v1") {
        const std::set<std::string> required_roles = {
            "fixture_graph", "segment_inventory", "pronunciation_lexicon",
            "lts_model", "model_token_inventory",
        };
        const std::set<std::string> optional_roles = {
            "heteronym_rules", "morphology_rules", "weak_form_rules",
        };
        for (const std::string &role : required_roles) {
            if (roles.find(role) == roles.end()) {
                invalid("fixture model is missing a required payload role");
            }
        }
        for (const std::string &role : roles) {
            if (required_roles.find(role) == required_roles.end() &&
                optional_roles.find(role) == optional_roles.end()) {
                invalid("fixture model declares an unsupported payload role");
            }
        }
        const auto graph = std::find_if(
            payloads.begin(), payloads.end(), [](const VerifiedPayload &payload) {
                return payload.role == "fixture_graph";
            });
        if (graph == payloads.end() || graph->path != "fixture.graph" ||
            graph->bytes.empty()) {
            invalid("fixture model is missing its deterministic fixture graph");
        }
    }
    if (total_bytes != expected_model_bytes) {
        invalid("declared model byte total does not match payload files");
    }
    model->payloads = std::move(payloads);
    *verified_paths = std::move(paths);
}

std::string shape_signature(const json::Value &value) {
    const auto &shape = array_value(value);
    if (shape.empty() || shape.size() > 8U) {
        invalid("tensor shape rank is invalid");
    }
    std::string result;
    for (const auto &dimension : shape) {
        if (!result.empty()) {
            result.push_back(',');
        }
        if (dimension.is_number()) {
            const std::uint64_t number = unsigned_value(dimension);
            if (number == 0U || number > 2147483647U) {
                invalid("fixed tensor dimension is invalid");
            }
            result += std::to_string(number);
        } else if (dimension.is_string()) {
            result += string_value(dimension, 32U);
        } else {
            invalid("tensor dimension has the wrong type");
        }
    }
    return result;
}

void verify_tensors(const json::Value &value, std::string_view engine_kind) {
    const auto &tensors = array_value(value);
    if (tensors.empty() || tensors.size() > 256U) {
        invalid("tensor metadata count is invalid");
    }
    std::set<std::string> signatures;
    for (const auto &entry : tensors) {
        const auto &tensor = object_value(entry);
        if (!exact_keys(tensor, {"name", "io", "dtype", "shape"})) {
            invalid("tensor metadata fields do not match schema v1");
        }
        const std::string signature = string_value(required(tensor, "name"), 127U) + "|" +
                                      string_value(required(tensor, "io"), 16U) + "|" +
                                      string_value(required(tensor, "dtype"), 16U) + "|" +
                                      shape_signature(required(tensor, "shape"));
        if (!signatures.insert(signature).second) {
            invalid("duplicate tensor metadata entry");
        }
    }
    if (engine_kind == "fixture-tone/v1") {
        const std::set<std::string> expected = {
            "audio|output|int16|T",
            "length_scale|input|float32|1",
            "speaker_id|input|int64|1",
            "token_ids|input|int64|1,N",
        };
        if (signatures != expected) {
            invalid("fixture graph tensor contract is unknown or incomplete");
        }
    }
}

void verify_licenses(const json::Value &value,
                     const std::set<std::string> &verified_paths) {
    const auto &licenses = array_value(value);
    if (licenses.empty() || licenses.size() > 128U) {
        invalid("model license record count is invalid");
    }
    for (const auto &entry : licenses) {
        const auto &license = object_value(entry);
        const bool without_notice = exact_keys(license, {"component", "license"});
        const bool with_notice = exact_keys(license, {"component", "license", "notice_path"});
        if (!without_notice && !with_notice) {
            invalid("model license fields do not match schema v1");
        }
        (void)string_value(required(license, "component"), 128U);
        (void)string_value(required(license, "license"), 128U);
        if (with_notice) {
            const std::string &path = string_value(required(license, "notice_path"), 255U);
            if (!safe_relative_path(path)) {
                invalid("model notice path is invalid");
            }
            if (verified_paths.find(path) == verified_paths.end()) {
                invalid("model notice path is not a verified payload file");
            }
        }
    }
}

void verify_determinism(const json::Value &value, VerifiedModel *model) {
    const auto &object = object_value(value);
    if (!exact_keys(object, {"class", "default_seed", "test_vector_sha256"})) {
        invalid("determinism fields do not match schema v1");
    }
    if (string_value(required(object, "class"), 64U) != "platform-independent-integer" ||
        string_value(required(object, "default_seed"), 20U) != "5433993625645500209") {
        invalid("fixture determinism contract is invalid");
    }
    model->deterministic_test_sha256 =
        string_value(required(object, "test_vector_sha256"));
    require_sha(model->deterministic_test_sha256);
}

std::uint64_t verify_resources(const json::Value &value) {
    const auto &object = object_value(value);
    if (!exact_keys(object, {"model_bytes", "minimum_memory_bytes"})) {
        invalid("resource fields do not match schema v1");
    }
    const std::uint64_t model_bytes = unsigned_value(required(object, "model_bytes"));
    (void)unsigned_value(required(object, "minimum_memory_bytes"));
    return model_bytes;
}

void verify_limitations(const json::Value &value) {
    const auto &limitations = array_value(value);
    if (limitations.size() > 128U) {
        invalid("model limitation count is invalid");
    }
    for (const auto &entry : limitations) {
        (void)string_value(entry, 512U);
    }
}

void verify_manifest(const json::Value &manifest,
                     const std::filesystem::path &root,
                     VerifiedModel *model) {
    const auto &object = object_value(manifest);
    if (!exact_keys(object, {"schema", "model_id", "version", "engine", "revisions",
                             "runtime_abi", "audio", "frontend", "voices", "quantization",
                             "required_cpu_features", "files", "tensors", "licenses",
                             "determinism", "resources", "limitations"})) {
        invalid("model manifest fields do not match schema v1");
    }
    if (string_value(required(object, "schema")) != "kilix.voicegen.model/v1") {
        unsupported_schema("model manifest schema is not supported");
    }
    model->model_id = string_value(required(object, "model_id"), 128U);
    model->version = string_value(required(object, "version"), 64U);

    const auto &engine = object_value(required(object, "engine"));
    if (!exact_keys(engine, {"kind"})) {
        invalid("model engine fields do not match schema v1");
    }
    model->engine_kind = string_value(required(engine, "kind"), 64U);
    if (model->engine_kind != "fixture-tone/v1") {
        invalid("model engine kind is not implemented by this runtime");
    }

    verify_revisions(required(object, "revisions"));
    verify_runtime_abi(required(object, "runtime_abi"));
    verify_audio(required(object, "audio"), model);
    verify_frontend(required(object, "frontend"), model);
    verify_voices(required(object, "voices"), model);
    verify_quantization(required(object, "quantization"), model->engine_kind);
    verify_cpu_features(required(object, "required_cpu_features"));
    const std::uint64_t model_bytes = verify_resources(required(object, "resources"));
    std::set<std::string> verified_paths;
    verify_files(required(object, "files"), root, model_bytes,
                 model->engine_kind, model, &verified_paths);
    verify_tensors(required(object, "tensors"), model->engine_kind);
    verify_licenses(required(object, "licenses"), verified_paths);
    verify_determinism(required(object, "determinism"), model);
    verify_limitations(required(object, "limitations"));
}

}  // namespace

const VerifiedPayload *VerifiedModel::payload_for_role(
    std::string_view role) const noexcept {
    const auto found = std::find_if(
        payloads.begin(), payloads.end(), [role](const VerifiedPayload &payload) {
            return payload.role == role;
        });
    return found == payloads.end() ? nullptr : &*found;
}

int verify_model_package(const std::filesystem::path &directory,
                         std::string_view expected_release_sha256,
                         VerifiedModel *model,
                         std::string *error) {
    if (model == nullptr || error == nullptr) {
        return KGV_INVALID_ARGUMENT;
    }
    *model = VerifiedModel{};
    error->clear();
    if (!is_lower_sha256(expected_release_sha256)) {
        *error = "expected release SHA-256 must be 64 lowercase hexadecimal characters";
        return KGV_INVALID_ARGUMENT;
    }

    try {
        std::error_code code;
        const std::filesystem::file_status root_status =
            std::filesystem::symlink_status(directory, code);
        if (code || !std::filesystem::is_directory(root_status) ||
            std::filesystem::is_symlink(root_status)) {
            io_error("model directory is absent, inaccessible, or a symbolic link");
        }
        require_regular_without_symlinks(directory, "RELEASE.json");

        std::uint64_t release_size = 0U;
        const std::string release_bytes =
            read_bounded(directory / "RELEASE.json", kMaximumReleaseBytes, &release_size);
        const std::string actual_release_sha = sha256_hex(release_bytes);
        if (actual_release_sha != expected_release_sha256) {
            mismatch("RELEASE.json SHA-256 does not match the caller-pinned value");
        }
        if (release_size == 0U || release_size > kMaximumReleaseBytes) {
            invalid("RELEASE.json size is invalid");
        }

        std::uint64_t declared_manifest_bytes = 0U;
        std::string declared_manifest_sha;
        verify_release(parse_metadata(release_bytes), &declared_manifest_bytes,
                       &declared_manifest_sha);

        require_regular_without_symlinks(directory, "MANIFEST.json");
        std::uint64_t manifest_size = 0U;
        const std::string manifest_bytes =
            read_bounded(directory / "MANIFEST.json", kMaximumManifestBytes,
                         &manifest_size);
        if (manifest_size != declared_manifest_bytes ||
            sha256_hex(manifest_bytes) != declared_manifest_sha) {
            mismatch("MANIFEST.json byte count or SHA-256 does not match RELEASE.json");
        }

        VerifiedModel verified;
        verified.directory = directory;
        verified.release_sha256 = actual_release_sha;
        verify_manifest(parse_metadata(manifest_bytes), directory, &verified);
        *model = std::move(verified);
        return KGV_OK;
    } catch (const ValidationError &caught) {
        *error = caught.what();
        return caught.status();
    } catch (const std::bad_alloc &) {
        *error = "model verification ran out of memory";
        return KGV_RESOURCE_EXHAUSTED;
    } catch (const std::exception &) {
        *error = "unexpected failure while verifying model package";
        return KGV_INTERNAL_ERROR;
    }
}

}  // namespace kgv
