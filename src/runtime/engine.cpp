#include "kilix_voicegen.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "frontend/frontend.h"
#include "runtime/frontend_package.h"
#include "runtime/model_package.h"
#include "runtime/sha256.h"

namespace {

constexpr std::size_t kMaximumModelDirectoryBytes = 4096U;
constexpr std::size_t kMaximumVoiceIdBytes = 64U;
constexpr std::size_t kMaximumReplacementBytes = 4096U;
constexpr std::size_t kMaximumOverrides = 256U;
constexpr std::uint32_t kMaximumThreads = 64U;
constexpr std::size_t kCallbackFrames = 480U;
constexpr std::size_t kEngineOptionsV1Size =
    offsetof(kgv_engine_options, expected_release_sha256) +
    sizeof(kgv_engine_options::expected_release_sha256);
constexpr std::size_t kRequestV1Size =
    offsetof(kgv_request, seed) + sizeof(kgv_request::seed);
constexpr std::size_t kFrontendInputV1Size =
    offsetof(kgv_frontend_input, dictionary) + sizeof(kgv_frontend_input::dictionary);
constexpr std::size_t kOverrideV1Size =
    offsetof(kgv_pronunciation_override, phone_segment_count) +
    sizeof(kgv_pronunciation_override::phone_segment_count);

struct CopiedOverride final {
    std::uint32_t byte_start = 0U;
    std::uint32_t byte_end = 0U;
    std::uint32_t kind = 0U;
    std::string replacement;
    std::vector<kgv_phone_segment> segments;
};

struct FinishRun;

void write_error(char *buffer, std::size_t size, std::string_view message) noexcept {
    if (buffer == nullptr || size == 0U) {
        return;
    }
    const std::size_t amount = std::min(size - 1U, message.size());
    if (amount > 0U) {
        std::memcpy(buffer, message.data(), amount);
    }
    buffer[amount] = '\0';
}

bool copy_c_string(const char *value, std::size_t maximum, std::string *result) {
    if (value == nullptr || result == nullptr) {
        return false;
    }
    std::size_t size = 0U;
    while (size <= maximum && value[size] != '\0') {
        ++size;
    }
    if (size == 0U || size > maximum) {
        return false;
    }
    result->assign(value, size);
    return true;
}

bool known_segment(const kgv::VerifiedModel &model, std::uint16_t id) {
    return std::binary_search(model.segment_ids.begin(), model.segment_ids.end(), id);
}

std::uint32_t phase_step_for_voice(std::string_view voice) noexcept {
    const std::uint64_t frequency = voice == "kilix-female-01" ? 196U : 124U;
    return static_cast<std::uint32_t>((frequency << 32U) / KGV_SAMPLE_RATE);
}

std::int16_t fixture_sample(std::uint32_t phase) noexcept {
    const std::uint32_t position = phase >> 16U;
    std::int32_t triangle = 0;
    if (position < 32768U) {
        triangle = static_cast<std::int32_t>(position * 2U) - 32768;
    } else {
        triangle = static_cast<std::int32_t>((65535U - position) * 2U) - 32768;
    }
    return static_cast<std::int16_t>((triangle * 900) / 32768);
}

std::uint64_t fixture_total_frames(std::size_t spoken_scalars,
                                   std::uint32_t rate_milli) noexcept {
    const std::size_t bounded = std::min<std::size_t>(spoken_scalars, 26U);
    const std::uint64_t base_frames =
        static_cast<std::uint64_t>(6U + bounded) * static_cast<std::uint64_t>(kCallbackFrames);
    return (base_frames * 1000U + static_cast<std::uint64_t>(rate_milli / 2U)) /
           static_cast<std::uint64_t>(rate_milli);
}

std::string fixture_smoke_sha256() {
    constexpr std::size_t kCanonicalSpokenScalars = 12U;
    const std::uint64_t total_frames = fixture_total_frames(kCanonicalSpokenScalars, 1000U);
    std::uint32_t phase = static_cast<std::uint32_t>(KGV_DEFAULT_SEED);
    const std::uint32_t step = phase_step_for_voice("kilix-female-01");
    kgv::Sha256 hash;
    for (std::uint64_t i = 0U; i < total_frames; ++i) {
        const std::int16_t sample = fixture_sample(phase);
        const std::uint16_t bits = static_cast<std::uint16_t>(sample);
        const std::uint8_t little_endian[2] = {
            static_cast<std::uint8_t>(bits & 0xffU),
            static_cast<std::uint8_t>(bits >> 8U),
        };
        hash.update(little_endian, sizeof(little_endian));
        phase += step;
    }
    return kgv::sha256_hex(hash.finish());
}

}  // namespace

struct kgv_engine final {
    std::atomic<unsigned int> references{1U};
    std::atomic<bool> closed{false};
    std::atomic<bool> claimed{false};
    std::uint32_t thread_count = 1U;
    kgv::VerifiedModel model;
    kgv::VerifiedFrontendPackage frontend;
};

struct kgv_job final {
    kgv_engine *engine = nullptr;
    std::string text;
    std::string voice_id;
    std::uint32_t profile = 0U;
    float rate = 1.0F;
    std::uint32_t rate_milli = 1000U;
    std::uint64_t seed = KGV_DEFAULT_SEED;
    kgv::FrontendAnalysis analysis;
    kgv::ResolvedFrontendResult resolved;
    std::atomic<bool> cancelled{false};
    std::atomic<int> state{0};  // 0 created, 1 running, 2 finished
    std::mutex state_mutex;
    std::condition_variable state_changed;
};

namespace {

void retain_engine(kgv_engine *engine) noexcept {
    (void)engine->references.fetch_add(1U, std::memory_order_relaxed);
}

void release_engine(kgv_engine *engine) noexcept {
    if (engine->references.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
        delete engine;
    }
}

struct FinishRun final {
    explicit FinishRun(kgv_job *value) : job(value) {}
    ~FinishRun() {
        job->state.store(2, std::memory_order_release);
        job->state_changed.notify_all();
    }
    kgv_job *job;
};

int copy_and_validate_overrides(const kgv_frontend_input &frontend,
                                const kgv::VerifiedModel &model,
                                std::string_view text,
                                std::vector<CopiedOverride> *output,
                                std::string *error) {
    if (frontend.override_count > kMaximumOverrides) {
        *error = "pronunciation override count exceeds the 256-entry limit";
        return KGV_INPUT_TOO_LARGE;
    }
    if (frontend.override_count > 0U && frontend.overrides == nullptr) {
        *error = "pronunciation override array is absent";
        return KGV_INVALID_ARGUMENT;
    }
    if (frontend.dictionary != nullptr) {
        *error = "dictionary handles are not available in the foundation runtime";
        return KGV_INVALID_ARGUMENT;
    }

    std::uint32_t previous_end = 0U;
    for (std::size_t index = 0U; index < frontend.override_count; ++index) {
        const kgv_pronunciation_override &entry = frontend.overrides[index];
        if (entry.struct_size < kOverrideV1Size) {
            *error = "pronunciation override struct is smaller than ABI v1";
            return KGV_ABI_MISMATCH;
        }
        if (entry.byte_start >= entry.byte_end ||
            static_cast<std::size_t>(entry.byte_end) > text.size() ||
            entry.byte_start < previous_end) {
            *error = "pronunciation override spans are invalid, unsorted, or overlapping";
            return KGV_INVALID_TEXT;
        }
        if (!kgv::is_utf8_boundary(text, entry.byte_start) ||
            !kgv::is_utf8_boundary(text, entry.byte_end)) {
            *error = "pronunciation override span does not end on UTF-8 boundaries";
            return KGV_INVALID_TEXT;
        }

        CopiedOverride copied;
        copied.byte_start = entry.byte_start;
        copied.byte_end = entry.byte_end;
        copied.kind = entry.kind;
        if (entry.kind == KGV_OVERRIDE_REPLACEMENT_TEXT) {
            if (entry.replacement_utf8 == nullptr || entry.replacement_utf8_size == 0U ||
                entry.replacement_utf8_size > kMaximumReplacementBytes ||
                entry.phone_segments != nullptr || entry.phone_segment_count != 0U) {
                *error = "replacement-text override fields are invalid";
                return KGV_INVALID_TEXT;
            }
            copied.replacement.assign(entry.replacement_utf8, entry.replacement_utf8_size);
            kgv::FrontendAnalysis replacement_analysis;
            kgv::FrontendFailure replacement_failure;
            const int status = kgv::analyze_frontend(copied.replacement, frontend.profile,
                                                     &replacement_analysis,
                                                     &replacement_failure);
            if (status != KGV_OK) {
                *error = "replacement-text override is not valid frontend input";
                return status;
            }
        } else if (entry.kind == KGV_OVERRIDE_PHONE_SYLLABLES) {
            if (entry.replacement_utf8 != nullptr || entry.replacement_utf8_size != 0U ||
                entry.phone_segments == nullptr || entry.phone_segment_count == 0U ||
                entry.phone_segment_count > 4096U) {
                *error = "phone-syllable override fields are invalid";
                return KGV_INVALID_TEXT;
            }
            copied.segments.assign(entry.phone_segments,
                                   entry.phone_segments + entry.phone_segment_count);
            for (std::size_t segment_index = 0U;
                 segment_index < copied.segments.size(); ++segment_index) {
                const kgv_phone_segment &segment = copied.segments[segment_index];
                if (!known_segment(model, segment.segment_id) ||
                    segment.syllable_start > 1U || segment.stress > 2U ||
                    (segment.syllable_start == 0U && segment.stress != 0U) ||
                    (segment_index == 0U && segment.syllable_start == 0U)) {
                    *error = "phone-syllable override contains an invalid segment or stress";
                    return KGV_INVALID_TEXT;
                }
            }
        } else {
            *error = "pronunciation override kind is unknown to ABI v1";
            return KGV_INVALID_ARGUMENT;
        }
        output->push_back(std::move(copied));
        previous_end = entry.byte_end;
    }
    return KGV_OK;
}

std::vector<kgv::RequestPronunciationOverride> resolved_overrides(
    const std::vector<CopiedOverride> &copied) {
    std::vector<kgv::RequestPronunciationOverride> result;
    result.reserve(copied.size());
    for (const CopiedOverride &entry : copied) {
        kgv::RequestPronunciationOverride converted;
        converted.span = {entry.byte_start, entry.byte_end};
        if (entry.kind == KGV_OVERRIDE_REPLACEMENT_TEXT) {
            converted.kind = kgv::RequestOverrideKind::replacement_text;
            converted.replacement_text = entry.replacement;
        } else {
            converted.kind = kgv::RequestOverrideKind::phone_syllables;
            for (const kgv_phone_segment &segment : entry.segments) {
                if (segment.syllable_start != 0U) {
                    kgv::PronunciationSyllable syllable;
                    if (segment.stress == 1U) {
                        syllable.stress = kgv::SyllableStress::primary;
                    } else if (segment.stress == 2U) {
                        syllable.stress = kgv::SyllableStress::secondary;
                    }
                    converted.syllables.push_back(std::move(syllable));
                }
                converted.syllables.back().segment_ids.push_back(segment.segment_id);
            }
        }
        result.push_back(std::move(converted));
    }
    return result;
}

}  // namespace

extern "C" {

uint32_t kgv_abi_version(void) {
    return KILIX_VOICEGEN_ABI_VERSION;
}

const char *kgv_status_name(int status) {
    switch (status) {
    case KGV_OK: return "KGV_OK";
    case KGV_CANCELLED: return "KGV_CANCELLED";
    case KGV_INVALID_ARGUMENT: return "KGV_INVALID_ARGUMENT";
    case KGV_INVALID_TEXT: return "KGV_INVALID_TEXT";
    case KGV_INVALID_VOICE: return "KGV_INVALID_VOICE";
    case KGV_INVALID_MODEL: return "KGV_INVALID_MODEL";
    case KGV_ABI_MISMATCH: return "KGV_ABI_MISMATCH";
    case KGV_BUSY: return "KGV_BUSY";
    case KGV_RESOURCE_EXHAUSTED: return "KGV_RESOURCE_EXHAUSTED";
    case KGV_INTERNAL_ERROR: return "KGV_INTERNAL_ERROR";
    case KGV_HASH_MISMATCH: return "KGV_HASH_MISMATCH";
    case KGV_UNSUPPORTED_SCHEMA: return "KGV_UNSUPPORTED_SCHEMA";
    case KGV_UNSUPPORTED_CPU: return "KGV_UNSUPPORTED_CPU";
    case KGV_IO_ERROR: return "KGV_IO_ERROR";
    case KGV_INPUT_TOO_LARGE: return "KGV_INPUT_TOO_LARGE";
    case KGV_INVALID_STATE: return "KGV_INVALID_STATE";
    default: return "KGV_UNKNOWN_STATUS";
    }
}

int kgv_engine_open(const char *model_directory,
                    const kgv_engine_options *options,
                    kgv_engine **out_engine,
                    char *error,
                    size_t error_size) {
    write_error(error, error_size, "");
    if (out_engine == nullptr) {
        write_error(error, error_size, "output engine pointer is required");
        return KGV_INVALID_ARGUMENT;
    }
    *out_engine = nullptr;
    if (model_directory == nullptr || options == nullptr) {
        write_error(error, error_size, "model directory and engine options are required");
        return KGV_INVALID_ARGUMENT;
    }
    if (options->struct_size < kEngineOptionsV1Size) {
        write_error(error, error_size, "engine options struct is smaller than ABI v1");
        return KGV_ABI_MISMATCH;
    }
    if (options->abi_version != KILIX_VOICEGEN_ABI_VERSION) {
        write_error(error, error_size, "requested runtime ABI is not supported");
        return KGV_ABI_MISMATCH;
    }
    if (options->flags != 0U) {
        write_error(error, error_size, "engine option flags must be zero in ABI v1");
        return KGV_ABI_MISMATCH;
    }
    if (options->thread_count == 0U || options->thread_count > kMaximumThreads) {
        write_error(error, error_size, "thread count must be between 1 and 64");
        return KGV_INVALID_ARGUMENT;
    }

    try {
        std::string directory;
        std::string expected_sha;
        if (!copy_c_string(model_directory, kMaximumModelDirectoryBytes, &directory)) {
            write_error(error, error_size, "model directory is empty or exceeds 4096 bytes");
            return KGV_INVALID_ARGUMENT;
        }
        if (!copy_c_string(options->expected_release_sha256, 64U, &expected_sha) ||
            expected_sha.size() != 64U) {
            write_error(error, error_size,
                        "expected release SHA-256 must be 64 lowercase hexadecimal characters");
            return KGV_INVALID_ARGUMENT;
        }

        kgv::VerifiedModel model;
        std::string verification_error;
        const int status = kgv::verify_model_package(directory, expected_sha, &model,
                                                     &verification_error);
        if (status != KGV_OK) {
            write_error(error, error_size, verification_error);
            return status;
        }
        if (model.deterministic_test_sha256 != fixture_smoke_sha256()) {
            write_error(error, error_size,
                        "fixture deterministic smoke-vector digest does not match runtime");
            return KGV_INVALID_MODEL;
        }

        kgv::VerifiedFrontendPackage frontend;
        std::string frontend_error;
        const int frontend_status = kgv::load_verified_frontend_package(
            model, &frontend, &frontend_error);
        if (frontend_status != KGV_OK) {
            write_error(error, error_size, frontend_error);
            return frontend_status;
        }

        auto engine = std::make_unique<kgv_engine>();
        engine->thread_count = options->thread_count;
        engine->model = std::move(model);
        engine->frontend = std::move(frontend);
        *out_engine = engine.release();
        return KGV_OK;
    } catch (const std::bad_alloc &) {
        write_error(error, error_size, "engine initialization ran out of memory");
        return KGV_RESOURCE_EXHAUSTED;
    } catch (const std::exception &) {
        write_error(error, error_size, "unexpected engine initialization failure");
        return KGV_INTERNAL_ERROR;
    }
}

int kgv_job_create(kgv_engine *engine,
                   const kgv_request *request,
                   kgv_job **out_job,
                   char *error,
                   size_t error_size) {
    write_error(error, error_size, "");
    if (out_job == nullptr) {
        write_error(error, error_size, "output job pointer is required");
        return KGV_INVALID_ARGUMENT;
    }
    *out_job = nullptr;
    if (engine == nullptr || request == nullptr) {
        write_error(error, error_size, "engine and request are required");
        return KGV_INVALID_ARGUMENT;
    }
    if (request->struct_size < kRequestV1Size ||
        request->frontend.struct_size < kFrontendInputV1Size) {
        write_error(error, error_size, "request struct is smaller than ABI v1");
        return KGV_ABI_MISMATCH;
    }
    if (engine->closed.load(std::memory_order_acquire)) {
        write_error(error, error_size, "engine has already been closed");
        return KGV_INVALID_STATE;
    }
    if (request->frontend.utf8_size > 0U && request->frontend.utf8_text == nullptr) {
        write_error(error, error_size, "UTF-8 input pointer is absent");
        return KGV_INVALID_ARGUMENT;
    }
    if (!std::isfinite(request->rate) || request->rate < 0.70F || request->rate > 1.50F) {
        write_error(error, error_size, "speech rate must be finite and between 0.70 and 1.50");
        return KGV_INVALID_ARGUMENT;
    }

    bool claimed = false;
    try {
        std::string text;
        if (request->frontend.utf8_size > 0U) {
            text.assign(request->frontend.utf8_text, request->frontend.utf8_size);
        }
        kgv::FrontendAnalysis analysis;
        kgv::FrontendFailure frontend_failure;
        int status = kgv::analyze_frontend(text, request->frontend.profile, &analysis,
                                           &frontend_failure);
        if (status != KGV_OK) {
            write_error(error, error_size, frontend_failure.message);
            return status;
        }

        std::string voice_id;
        if (!copy_c_string(request->voice_id, kMaximumVoiceIdBytes, &voice_id)) {
            write_error(error, error_size, "voice ID is empty or exceeds 64 bytes");
            return KGV_INVALID_VOICE;
        }
        if (std::find(engine->model.voice_ids.begin(), engine->model.voice_ids.end(),
                      voice_id) == engine->model.voice_ids.end()) {
            write_error(error, error_size, "voice ID is not present in the verified model");
            return KGV_INVALID_VOICE;
        }

        std::vector<CopiedOverride> overrides;
        overrides.reserve(request->frontend.override_count);
        status = copy_and_validate_overrides(request->frontend, engine->model, text,
                                             &overrides, &frontend_failure.message);
        if (status != KGV_OK) {
            write_error(error, error_size, frontend_failure.message);
            return status;
        }

        const std::vector<kgv::RequestPronunciationOverride> request_overrides =
            resolved_overrides(overrides);
        kgv::ResolvedFrontendResult resolved;
        kgv::ResolvedFrontendFailure resolved_failure;
        static const std::vector<std::string> no_word_roles;
        status = kgv::run_resolved_frontend(
            text, request->frontend.profile, engine->frontend.resources(),
            no_word_roles, request_overrides, &resolved, &resolved_failure);
        if (status != KGV_OK) {
            write_error(error, error_size, resolved_failure.message);
            return status;
        }

        bool expected = false;
        if (!engine->claimed.compare_exchange_strong(expected, true,
                                                     std::memory_order_acq_rel)) {
            write_error(error, error_size, "engine already owns an active job");
            return KGV_BUSY;
        }
        claimed = true;
        retain_engine(engine);

        auto job = std::make_unique<kgv_job>();
        job->engine = engine;
        job->text = std::move(text);
        job->voice_id = std::move(voice_id);
        job->profile = request->frontend.profile;
        job->rate = request->rate;
        job->rate_milli = static_cast<std::uint32_t>(
            std::lround(static_cast<double>(request->rate) * 1000.0));
        job->seed = request->seed;
        job->analysis = analysis;
        job->resolved = std::move(resolved);
        *out_job = job.release();
        return KGV_OK;
    } catch (const std::bad_alloc &) {
        if (claimed) {
            engine->claimed.store(false, std::memory_order_release);
            release_engine(engine);
        }
        write_error(error, error_size, "job creation ran out of memory");
        return KGV_RESOURCE_EXHAUSTED;
    } catch (const std::exception &) {
        if (claimed) {
            engine->claimed.store(false, std::memory_order_release);
            release_engine(engine);
        }
        write_error(error, error_size, "unexpected job creation failure");
        return KGV_INTERNAL_ERROR;
    }
}

int kgv_job_run(kgv_job *job,
                kgv_pcm_callback callback,
                void *user,
                char *error,
                size_t error_size) {
    write_error(error, error_size, "");
    if (job == nullptr || callback == nullptr) {
        write_error(error, error_size, "job and PCM callback are required");
        return KGV_INVALID_ARGUMENT;
    }
    int expected = 0;
    if (!job->state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        if (expected == 1) {
            write_error(error, error_size, "job is already running");
            return KGV_BUSY;
        }
        write_error(error, error_size, "job has already completed");
        return KGV_INVALID_STATE;
    }
    FinishRun finish(job);

    if (job->cancelled.load(std::memory_order_acquire)) {
        write_error(error, error_size, "synthesis was cancelled before it started");
        return KGV_CANCELLED;
    }
    if (job->analysis.spoken_scalar_count == 0U) {
        return KGV_OK;
    }

    try {
        const std::uint64_t total_frames =
            fixture_total_frames(job->analysis.spoken_scalar_count, job->rate_milli);
        std::uint64_t emitted = 0U;
        std::uint32_t phase = static_cast<std::uint32_t>(job->seed);
        const std::uint32_t step = phase_step_for_voice(job->voice_id);
        std::vector<std::int16_t> block(kCallbackFrames);

        while (emitted < total_frames) {
            if (job->cancelled.load(std::memory_order_acquire)) {
                write_error(error, error_size, "synthesis was cancelled");
                return KGV_CANCELLED;
            }
            const std::uint64_t remaining = total_frames - emitted;
            const std::size_t frame_count = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, kCallbackFrames));
            for (std::size_t i = 0U; i < frame_count; ++i) {
                block[i] = fixture_sample(phase);
                phase += step;
            }
            if (callback(block.data(), frame_count, job->engine->model.sample_rate, user) == 0) {
                job->cancelled.store(true, std::memory_order_release);
                write_error(error, error_size, "synthesis was cancelled by the PCM consumer");
                return KGV_CANCELLED;
            }
            emitted += static_cast<std::uint64_t>(frame_count);
        }
        return KGV_OK;
    } catch (const std::bad_alloc &) {
        write_error(error, error_size, "synthesis ran out of memory");
        return KGV_RESOURCE_EXHAUSTED;
    } catch (const std::exception &) {
        write_error(error, error_size, "unexpected synthesis runtime failure");
        return KGV_INTERNAL_ERROR;
    }
}

void kgv_job_cancel(kgv_job *job) {
    if (job != nullptr) {
        job->cancelled.store(true, std::memory_order_release);
    }
}

void kgv_job_destroy(kgv_job *job) {
    if (job == nullptr) {
        return;
    }
    job->cancelled.store(true, std::memory_order_release);
    {
        std::unique_lock<std::mutex> lock(job->state_mutex);
        job->state_changed.wait(lock, [job] {
            return job->state.load(std::memory_order_acquire) != 1;
        });
    }
    kgv_engine *engine = job->engine;
    engine->claimed.store(false, std::memory_order_release);
    delete job;
    release_engine(engine);
}

void kgv_engine_close(kgv_engine *engine) {
    if (engine == nullptr) {
        return;
    }
    bool expected = false;
    if (engine->closed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        release_engine(engine);
    }
}

}  // extern "C"
