#include "runtime/frontend_package.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "kilix_voicegen.h"
#include "runtime/sha256.h"

namespace kgv {
namespace {

int reject(VerifiedFrontendPackage *frontend,
           std::string *error,
           int status,
           std::string message) {
    *frontend = VerifiedFrontendPackage{};
    *error = std::move(message);
    return status;
}

bool stable_segment_name(std::string_view value) noexcept {
    if (value.empty() || value.size() > 32U || value.front() < 'A' ||
        value.front() > 'Z') {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '_';
    });
}

int parse_segments(std::string_view bytes,
                   std::string_view expected_sha256,
                   const std::vector<std::uint16_t> &expected_ids,
                   std::vector<SegmentDefinition> *segments,
                   std::string *error) {
    segments->clear();
    if (bytes.empty() || bytes.size() > 4U * 1024U * 1024U ||
        bytes.back() != '\n' || bytes.find('\r') != std::string_view::npos ||
        bytes.find('\0') != std::string_view::npos) {
        *error = "segment inventory is empty, oversized, or not canonical LF text";
        return KGV_INVALID_MODEL;
    }
    std::set<std::string, std::less<>> names;
    std::uint16_t previous_id = 0U;
    std::size_t cursor = 0U;
    while (cursor < bytes.size()) {
        const std::size_t end = bytes.find('\n', cursor);
        if (end == std::string_view::npos || end == cursor) {
            segments->clear();
            *error = "segment inventory contains an empty or unterminated line";
            return KGV_INVALID_MODEL;
        }
        const std::string_view line = bytes.substr(cursor, end - cursor);
        const std::size_t tab = line.find('\t');
        if (tab == std::string_view::npos || tab == 0U ||
            line.find('\t', tab + 1U) != std::string_view::npos) {
            segments->clear();
            *error = "segment inventory line must contain one ID/name separator";
            return KGV_INVALID_MODEL;
        }
        const std::string_view digits = line.substr(0U, tab);
        const std::string_view name = line.substr(tab + 1U);
        if (digits.size() > 5U || digits.front() == '0' ||
            !stable_segment_name(name)) {
            segments->clear();
            *error = "segment inventory contains a non-canonical ID or name";
            return KGV_INVALID_MODEL;
        }
        std::uint32_t raw_id = 0U;
        const auto parsed = std::from_chars(digits.data(),
                                            digits.data() + digits.size(), raw_id);
        if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() ||
            raw_id == 0U || raw_id > 65535U || raw_id <= previous_id ||
            !names.insert(std::string(name)).second) {
            segments->clear();
            *error = "segment inventory IDs and names must be unique and ascending";
            return KGV_INVALID_MODEL;
        }
        segments->push_back(
            SegmentDefinition{std::string(name), static_cast<std::uint16_t>(raw_id)});
        previous_id = static_cast<std::uint16_t>(raw_id);
        cursor = end + 1U;
    }
    if (segments->empty() || segments->size() != expected_ids.size()) {
        segments->clear();
        *error = "segment inventory does not match the manifest ID count";
        return KGV_ABI_MISMATCH;
    }
    for (std::size_t index = 0U; index < segments->size(); ++index) {
        if ((*segments)[index].id != expected_ids[index]) {
            segments->clear();
            *error = "segment inventory IDs do not match the manifest";
            return KGV_ABI_MISMATCH;
        }
    }
    const std::string canonical_sha =
        pronunciation_segment_inventory_sha256(*segments);
    if (canonical_sha.empty() || canonical_sha != expected_sha256 ||
        sha256_hex(bytes) != expected_sha256) {
        segments->clear();
        *error = "segment inventory canonical bytes do not match the frontend binding";
        return KGV_ABI_MISMATCH;
    }
    return KGV_OK;
}

const VerifiedPayload *required_payload(const VerifiedModel &model,
                                        std::string_view role,
                                        std::string *error) {
    const VerifiedPayload *payload = model.payload_for_role(role);
    if (payload == nullptr) {
        *error = "verified model lacks required frontend payload role " +
                 std::string(role);
    }
    return payload;
}

std::string resource_failure(std::string_view kind,
                             std::string_view code,
                             std::string_view message) {
    std::string result(kind);
    result += " resource rejected";
    if (!code.empty()) {
        result += " (";
        result += code;
        result += ')';
    }
    if (!message.empty()) {
        result += ": ";
        result += message;
    }
    return result;
}

}  // namespace

ResolvedFrontendResources VerifiedFrontendPackage::resources() const noexcept {
    ResolvedFrontendResources result;
    result.base_lexicon = &base_lexicon;
    result.heteronym_rules = heteronym_rules ? &*heteronym_rules : nullptr;
    result.morphology_rules = morphology_rules ? &*morphology_rules : nullptr;
    result.weak_form_rules = weak_form_rules ? &*weak_form_rules : nullptr;
    result.lts = &lts;
    result.model_tokens = &model_tokens;
    result.required_admission = admission;
    result.expected_frontend_abi_sha256 = frontend_abi_sha256;
    return result;
}

int load_verified_frontend_package(const VerifiedModel &model,
                                   VerifiedFrontendPackage *frontend,
                                   std::string *error) {
    if (frontend == nullptr || error == nullptr) {
        return KGV_INVALID_ARGUMENT;
    }
    *frontend = VerifiedFrontendPackage{};
    error->clear();

    VerifiedFrontendPackage candidate;
    if (model.frontend_admission == "product-admitted") {
        candidate.admission = PronunciationAdmission::product_admitted;
    } else if (model.frontend_admission == "test-fixture") {
        candidate.admission = PronunciationAdmission::test_fixture;
    } else {
        return reject(frontend, error, KGV_INVALID_MODEL,
                      "verified model has an unsupported frontend admission");
    }
    if (!is_lower_sha256(model.frontend_abi_sha256)) {
        return reject(frontend, error, KGV_INVALID_MODEL,
                      "verified model lacks a valid frontend ABI binding");
    }
    candidate.frontend_abi_sha256 = model.frontend_abi_sha256;

    const VerifiedPayload *segments =
        required_payload(model, "segment_inventory", error);
    const VerifiedPayload *lexicon =
        required_payload(model, "pronunciation_lexicon", error);
    const VerifiedPayload *lts = required_payload(model, "lts_model", error);
    const VerifiedPayload *tokens =
        required_payload(model, "model_token_inventory", error);
    if (segments == nullptr || lexicon == nullptr || lts == nullptr ||
        tokens == nullptr) {
        return reject(frontend, error, KGV_INVALID_MODEL, *error);
    }

    int status = parse_segments(segments->bytes,
                                model.segment_inventory_sha256,
                                model.segment_ids, &candidate.segments, error);
    if (status != KGV_OK) {
        return reject(frontend, error, status, *error);
    }

    PronunciationResourceFailure pronunciation_failure;
    status = load_pronunciation_lexicon(
        lexicon->bytes, lexicon->sha256, model.segment_inventory_sha256,
        candidate.admission, candidate.segments, &candidate.base_lexicon,
        &pronunciation_failure);
    if (status != KGV_OK) {
        return reject(frontend, error, status,
                      resource_failure("pronunciation",
                                       pronunciation_failure.code,
                                       pronunciation_failure.message));
    }

    LtsFailure lts_failure;
    status = load_lts_model(lts->bytes, lts->sha256,
                            model.segment_inventory_sha256,
                            candidate.admission, candidate.segments,
                            &candidate.lts, &lts_failure);
    if (status != KGV_OK) {
        return reject(frontend, error, status,
                      resource_failure("LTS", lts_failure.code,
                                       lts_failure.message));
    }

    ModelTokenFailure token_failure;
    status = load_model_token_inventory(
        tokens->bytes, tokens->sha256, model.segment_inventory_sha256,
        candidate.admission, candidate.segments, &candidate.model_tokens,
        &token_failure);
    if (status != KGV_OK) {
        return reject(frontend, error, status,
                      resource_failure("model-token", token_failure.code,
                                       token_failure.message));
    }

    if (const VerifiedPayload *payload =
            model.payload_for_role("heteronym_rules")) {
        HeteronymRules rules;
        HeteronymResourceFailure failure;
        status = load_heteronym_rules(
            payload->bytes, payload->sha256, lexicon->sha256,
            candidate.admission, &rules, &failure);
        if (status != KGV_OK) {
            return reject(frontend, error, status,
                          resource_failure("heteronym", failure.code,
                                           failure.message));
        }
        candidate.heteronym_rules = std::move(rules);
    }
    if (const VerifiedPayload *payload =
            model.payload_for_role("morphology_rules")) {
        MorphologyRules rules;
        MorphologyResourceFailure failure;
        status = load_morphology_rules(
            payload->bytes, payload->sha256, lexicon->sha256,
            model.segment_inventory_sha256, candidate.admission,
            candidate.segments, &rules, &failure);
        if (status != KGV_OK) {
            return reject(frontend, error, status,
                          resource_failure("morphology", failure.code,
                                           failure.message));
        }
        candidate.morphology_rules = std::move(rules);
    }
    if (const VerifiedPayload *payload =
            model.payload_for_role("weak_form_rules")) {
        WeakFormRules rules;
        WeakFormResourceFailure failure;
        status = load_weak_form_rules(
            payload->bytes, payload->sha256, lexicon->sha256,
            model.segment_inventory_sha256, candidate.admission,
            candidate.segments, &rules, &failure);
        if (status != KGV_OK) {
            return reject(frontend, error, status,
                          resource_failure("weak-form", failure.code,
                                           failure.message));
        }
        candidate.weak_form_rules = std::move(rules);
    }

    ResolvedFrontendResult probe;
    ResolvedFrontendFailure probe_failure;
    static const std::vector<std::string> no_roles;
    status = run_resolved_frontend("", KGV_PROFILE_PROSE,
                                   candidate.resources(), no_roles,
                                   &probe, &probe_failure);
    if (status != KGV_OK) {
        return reject(frontend, error, status,
                      resource_failure("resolved frontend",
                                       probe_failure.code,
                                       probe_failure.message));
    }

    *frontend = std::move(candidate);
    return KGV_OK;
}

}  // namespace kgv
