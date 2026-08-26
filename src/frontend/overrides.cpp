#include "frontend/overrides.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "frontend/frontend.h"
#include "kilix_voicegen.h"

namespace kgv {
namespace {

constexpr std::size_t kMaximumOverrides = 256U;
constexpr std::size_t kMaximumReplacementBytes = 4096U;
constexpr std::size_t kMaximumSyllables = 16U;
constexpr std::size_t kMaximumSegmentsPerSyllable = 32U;
constexpr std::size_t kMaximumSegmentsPerWord = 128U;

struct SpanMapSegment final {
    SourceSpan output;
    SourceSpan source;
    std::optional<std::size_t> replacement_override;
};

int reject(OverrideLexicalResult *result,
           RequestOverrideFailure *failure,
           int status,
           std::string code,
           std::string message,
           SourceSpan span = {},
           std::size_t override_index = 0U,
           bool has_override = false) {
    if (result != nullptr) {
        *result = OverrideLexicalResult{};
    }
    if (failure != nullptr) {
        failure->status = status;
        failure->code = std::move(code);
        failure->message = std::move(message);
        failure->span = span;
        failure->override_index = override_index;
        failure->has_override = has_override;
    }
    return status;
}

bool overlaps(SourceSpan left, SourceSpan right) noexcept {
    return left.byte_start < right.byte_end &&
           right.byte_start < left.byte_end;
}

bool same_span(SourceSpan left, SourceSpan right) noexcept {
    return left.byte_start == right.byte_start &&
           left.byte_end == right.byte_end;
}

void append_map_segment(std::vector<SpanMapSegment> *segments,
                        std::size_t output_start,
                        std::size_t output_end,
                        SourceSpan source,
                        std::optional<std::size_t> replacement_override) {
    if (output_start == output_end) {
        return;
    }
    segments->push_back(SpanMapSegment{
        SourceSpan{output_start, output_end}, source, replacement_override,
    });
}

SourceSpan mapped_piece(const SpanMapSegment &segment,
                        std::size_t begin,
                        std::size_t end) noexcept {
    if (segment.replacement_override.has_value()) {
        return segment.source;
    }
    return SourceSpan{
        segment.source.byte_start + (begin - segment.output.byte_start),
        segment.source.byte_start + (end - segment.output.byte_start),
    };
}

SourceSpan map_output_span(
    SourceSpan output,
    const std::vector<SpanMapSegment> &segments,
    std::vector<std::size_t> *replacement_indices = nullptr) {
    if (replacement_indices != nullptr) {
        replacement_indices->clear();
    }
    std::size_t mapped_start = std::numeric_limits<std::size_t>::max();
    std::size_t mapped_end = 0U;
    for (const SpanMapSegment &segment : segments) {
        const std::size_t begin =
            std::max(output.byte_start, segment.output.byte_start);
        const std::size_t end = std::min(output.byte_end,
                                         segment.output.byte_end);
        if (begin >= end) {
            continue;
        }
        const SourceSpan piece = mapped_piece(segment, begin, end);
        mapped_start = std::min(mapped_start, piece.byte_start);
        mapped_end = std::max(mapped_end, piece.byte_end);
        if (replacement_indices != nullptr &&
            segment.replacement_override.has_value() &&
            (replacement_indices->empty() ||
             replacement_indices->back() !=
                 *segment.replacement_override)) {
            replacement_indices->push_back(
                *segment.replacement_override);
        }
    }
    if (mapped_start == std::numeric_limits<std::size_t>::max()) {
        return SourceSpan{};
    }
    return SourceSpan{mapped_start, mapped_end};
}

SourceSpan map_output_offset(
    std::size_t output_offset,
    const std::vector<SpanMapSegment> &segments,
    std::size_t source_size) noexcept {
    for (const SpanMapSegment &segment : segments) {
        if (output_offset >= segment.output.byte_start &&
            output_offset < segment.output.byte_end) {
            if (segment.replacement_override.has_value()) {
                return segment.source;
            }
            const std::size_t mapped = segment.source.byte_start +
                (output_offset - segment.output.byte_start);
            return SourceSpan{mapped, std::min(source_size, mapped + 1U)};
        }
    }
    if (output_offset == 0U) {
        return SourceSpan{};
    }
    return SourceSpan{source_size, source_size};
}

bool valid_phone_override(
    const RequestPronunciationOverride &entry,
    const ModelTokenInventory &model_tokens,
    std::string *code,
    std::string *message) {
    if (!entry.replacement_text.empty() || entry.syllables.empty() ||
        entry.syllables.size() > kMaximumSyllables) {
        *code = "INVALID_PHONE_OVERRIDE";
        *message = "phone override fields or syllable count are invalid";
        return false;
    }
    std::size_t segment_count = 0U;
    std::size_t primary_count = 0U;
    for (const PronunciationSyllable &syllable : entry.syllables) {
        if (syllable.stress != SyllableStress::none &&
            syllable.stress != SyllableStress::primary &&
            syllable.stress != SyllableStress::secondary) {
            *code = "INVALID_PHONE_OVERRIDE";
            *message = "phone override syllable stress is invalid";
            return false;
        }
        if (syllable.segment_ids.empty() ||
            syllable.segment_ids.size() > kMaximumSegmentsPerSyllable) {
            *code = "INVALID_PHONE_OVERRIDE";
            *message = "phone override syllable segment count is invalid";
            return false;
        }
        if (syllable.stress == SyllableStress::primary &&
            ++primary_count > 1U) {
            *code = "INVALID_PHONE_OVERRIDE";
            *message = "phone override has multiple primary stresses";
            return false;
        }
        segment_count += syllable.segment_ids.size();
        if (segment_count > kMaximumSegmentsPerWord) {
            *code = "INVALID_PHONE_OVERRIDE";
            *message = "phone override exceeds the segment limit";
            return false;
        }
        for (std::uint16_t segment_id : syllable.segment_ids) {
            if (!model_tokens.segment_token_id(segment_id).has_value()) {
                *code = "UNKNOWN_OVERRIDE_SEGMENT";
                *message = "phone override uses a segment outside the model inventory";
                return false;
            }
        }
    }
    return true;
}

int convert_lexical_failure(
    const FrontendFailure &frontend_failure,
    const std::vector<SpanMapSegment> &segments,
    std::size_t source_size,
    OverrideLexicalResult *result,
    RequestOverrideFailure *failure) {
    const SourceSpan span = map_output_offset(
        frontend_failure.byte_offset, segments, source_size);
    std::size_t override_index = 0U;
    bool has_override = false;
    for (const SpanMapSegment &segment : segments) {
        if (segment.replacement_override.has_value() &&
            overlaps(segment.source, span)) {
            override_index = *segment.replacement_override;
            has_override = true;
            break;
        }
    }
    return reject(result, failure, frontend_failure.status,
                  frontend_failure.code, frontend_failure.message, span,
                  override_index, has_override);
}

}  // namespace

int run_override_lexical_frontend(
    std::string_view text,
    std::uint32_t profile,
    const std::vector<RequestPronunciationOverride> &overrides,
    const ModelTokenInventory &model_tokens,
    OverrideLexicalResult *result,
    RequestOverrideFailure *failure) {
    if (result == nullptr || failure == nullptr) {
        return reject(result, failure, KGV_INVALID_ARGUMENT,
                      "INVALID_OVERRIDE_ARGUMENT",
                      "override processing requires output records");
    }
    *result = OverrideLexicalResult{};
    *failure = RequestOverrideFailure{};
    if (overrides.size() > kMaximumOverrides) {
        return reject(result, failure, KGV_INPUT_TOO_LARGE,
                      "TOO_MANY_OVERRIDES",
                      "request exceeds the 256-entry override limit");
    }
    if (overrides.empty()) {
        FrontendFailure frontend_failure;
        const int status = run_lexical_frontend(
            text, profile, &result->lexical, &frontend_failure);
        if (status != KGV_OK) {
            return reject(result, failure, status, frontend_failure.code,
                          frontend_failure.message,
                          SourceSpan{frontend_failure.byte_offset,
                                     std::min(text.size(),
                                              frontend_failure.byte_offset +
                                                  1U)});
        }
        result->phone_override_by_word.resize(result->lexical.words.size());
        result->replacement_override_by_word.resize(
            result->lexical.words.size());
        return KGV_OK;
    }

    FrontendAnalysis original_analysis;
    FrontendFailure original_failure;
    const int original_status = analyze_frontend(
        text, profile, &original_analysis, &original_failure);
    if (original_status != KGV_OK) {
        return reject(result, failure, original_status,
                      original_failure.code, original_failure.message,
                      SourceSpan{original_failure.byte_offset,
                                 std::min(text.size(),
                                          original_failure.byte_offset + 1U)});
    }
    std::vector<FrontendScalar> normalized_scalars;
    const int normalization_status = normalize_frontend_nfc(
        text, original_analysis.visible_scalars, &normalized_scalars,
        &original_failure);
    if (normalization_status != KGV_OK) {
        return reject(result, failure, normalization_status,
                      original_failure.code, original_failure.message,
                      SourceSpan{original_failure.byte_offset,
                                 std::min(text.size(),
                                          original_failure.byte_offset + 1U)});
    }
    std::vector<std::size_t> grapheme_boundaries = {0U, text.size()};
    for (const FrontendScalar &scalar : normalized_scalars) {
        grapheme_boundaries.push_back(scalar.byte_start);
        grapheme_boundaries.push_back(scalar.byte_end);
    }
    for (const FrontendControlSequence &control :
         original_analysis.control_sequences) {
        grapheme_boundaries.push_back(control.byte_start);
        grapheme_boundaries.push_back(control.byte_end);
    }
    std::sort(grapheme_boundaries.begin(), grapheme_boundaries.end());
    grapheme_boundaries.erase(
        std::unique(grapheme_boundaries.begin(), grapheme_boundaries.end()),
        grapheme_boundaries.end());

    std::size_t previous_start = 0U;
    std::size_t previous_end = 0U;
    bool saw_override = false;
    for (std::size_t index = 0U; index < overrides.size(); ++index) {
        const RequestPronunciationOverride &entry = overrides[index];
        if (entry.span.byte_start >= entry.span.byte_end ||
            entry.span.byte_end > text.size()) {
            return reject(result, failure, KGV_INVALID_TEXT,
                          "INVALID_OVERRIDE_SPAN",
                          "request override span is empty or outside the input",
                          entry.span, index, true);
        }
        if (saw_override && entry.span.byte_start < previous_end) {
            return reject(result, failure, KGV_INVALID_TEXT,
                          "OVERLAPPING_OVERRIDE",
                          "request overrides are unsorted or overlap",
                          entry.span, index, true);
        }
        if ((saw_override && entry.span.byte_start < previous_start) ||
            !std::binary_search(grapheme_boundaries.begin(),
                                grapheme_boundaries.end(),
                                entry.span.byte_start) ||
            !std::binary_search(grapheme_boundaries.begin(),
                                grapheme_boundaries.end(),
                                entry.span.byte_end)) {
            return reject(result, failure, KGV_INVALID_TEXT,
                          "INVALID_OVERRIDE_BOUNDARY",
                          "request override must end on grapheme boundaries",
                          entry.span, index, true);
        }
        if (entry.kind == RequestOverrideKind::replacement_text) {
            if (entry.replacement_text.empty() ||
                entry.replacement_text.size() > kMaximumReplacementBytes ||
                !entry.syllables.empty()) {
                return reject(result, failure, KGV_INVALID_TEXT,
                              "INVALID_REPLACEMENT_OVERRIDE",
                              "replacement override fields are invalid",
                              entry.span, index, true);
            }
        } else if (entry.kind == RequestOverrideKind::phone_syllables) {
            std::string code;
            std::string message;
            if (!valid_phone_override(entry, model_tokens, &code, &message)) {
                return reject(result, failure, KGV_INVALID_TEXT,
                              std::move(code), std::move(message),
                              entry.span, index, true);
            }
        } else {
            return reject(result, failure, KGV_INVALID_ARGUMENT,
                          "UNKNOWN_OVERRIDE_KIND",
                          "request override kind is unsupported",
                          entry.span, index, true);
        }
        previous_start = entry.span.byte_start;
        previous_end = entry.span.byte_end;
        saw_override = true;
    }

    std::string transformed;
    transformed.reserve(text.size());
    std::vector<SpanMapSegment> span_map;
    span_map.reserve(overrides.size() * 2U + 1U);
    std::size_t source_cursor = 0U;
    for (std::size_t index = 0U; index < overrides.size(); ++index) {
        const RequestPronunciationOverride &entry = overrides[index];
        const std::size_t prefix_start = transformed.size();
        transformed.append(text.substr(source_cursor,
                                       entry.span.byte_start - source_cursor));
        append_map_segment(&span_map, prefix_start, transformed.size(),
                           SourceSpan{source_cursor, entry.span.byte_start},
                           std::nullopt);
        const std::size_t entry_start = transformed.size();
        if (entry.kind == RequestOverrideKind::replacement_text) {
            transformed.append(entry.replacement_text);
            append_map_segment(&span_map, entry_start, transformed.size(),
                               entry.span, index);
        } else {
            transformed.append(text.substr(
                entry.span.byte_start,
                entry.span.byte_end - entry.span.byte_start));
            append_map_segment(&span_map, entry_start, transformed.size(),
                               entry.span, std::nullopt);
        }
        source_cursor = entry.span.byte_end;
    }
    const std::size_t suffix_start = transformed.size();
    transformed.append(text.substr(source_cursor));
    append_map_segment(&span_map, suffix_start, transformed.size(),
                       SourceSpan{source_cursor, text.size()}, std::nullopt);
    if (transformed.size() > KGV_MAX_INPUT_BYTES) {
        return reject(result, failure, KGV_INPUT_TOO_LARGE,
                      "OVERRIDE_EXPANDED_INPUT_TOO_LARGE",
                      "replacement overrides expand beyond the input limit");
    }

    FrontendFailure transformed_failure;
    const int transformed_status = run_lexical_frontend(
        transformed, profile, &result->lexical, &transformed_failure);
    if (transformed_status != KGV_OK) {
        return convert_lexical_failure(transformed_failure, span_map,
                                       text.size(), result, failure);
    }

    result->replacement_override_by_word.resize(result->lexical.words.size());
    std::vector<std::size_t> replacement_indices;
    for (std::size_t index = 0U; index < result->lexical.words.size(); ++index) {
        LexicalWord &word = result->lexical.words[index];
        const SourceSpan mapped = map_output_span(
            word.span, span_map, &replacement_indices);
        if (mapped.byte_start >= mapped.byte_end ||
            replacement_indices.size() > 1U) {
            return reject(result, failure, KGV_INVALID_TEXT,
                          "AMBIGUOUS_REPLACEMENT_MAPPING",
                          "one spoken word crosses multiple request replacements",
                          mapped,
                          replacement_indices.empty()
                              ? 0U
                              : replacement_indices.front(),
                          !replacement_indices.empty());
        }
        word.span = mapped;
        if (!replacement_indices.empty()) {
            result->replacement_override_by_word[index] =
                replacement_indices.front();
        }
    }
    for (LexicalPhrase &phrase : result->lexical.phrases) {
        phrase.span = map_output_span(phrase.span, span_map);
    }
    for (FrontendDiagnostic &diagnostic : result->lexical.diagnostics) {
        diagnostic.span = map_output_span(diagnostic.span, span_map);
    }
    result->lexical.input_bytes = text.size();

    result->phone_override_by_word.resize(result->lexical.words.size());
    for (std::size_t override_index = 0U;
         override_index < overrides.size(); ++override_index) {
        const RequestPronunciationOverride &entry =
            overrides[override_index];
        if (entry.kind != RequestOverrideKind::phone_syllables) {
            continue;
        }
        std::size_t match = 0U;
        std::size_t match_count = 0U;
        bool partial_intersection = false;
        for (std::size_t word_index = 0U;
             word_index < result->lexical.words.size(); ++word_index) {
            const SourceSpan word_span =
                result->lexical.words[word_index].span;
            if (same_span(entry.span, word_span)) {
                match = word_index;
                ++match_count;
            } else if (overlaps(entry.span, word_span)) {
                partial_intersection = true;
            }
        }
        if (match_count != 1U || partial_intersection) {
            return reject(result, failure, KGV_INVALID_TEXT,
                          "INVALID_PHONE_OVERRIDE_TARGET",
                          "phone override must match exactly one spoken word",
                          entry.span, override_index, true);
        }
        result->phone_override_by_word[match] = override_index;
    }
    return KGV_OK;
}

}  // namespace kgv
