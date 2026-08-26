#ifndef KILIX_VOICEGEN_H
#define KILIX_VOICEGEN_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(KGV_BUILDING_LIBRARY)
#    define KGV_API __declspec(dllexport)
#  else
#    define KGV_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define KGV_API __attribute__((visibility("default")))
#else
#  define KGV_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define KILIX_VOICEGEN_ABI_VERSION 1u
#define KGV_SAMPLE_RATE 24000u
#define KGV_MAX_INPUT_BYTES 65536u
#define KGV_DEFAULT_SEED UINT64_C(0x4b696c6978564731)

typedef struct kgv_engine kgv_engine;
typedef struct kgv_job kgv_job;
typedef struct kgv_dictionary kgv_dictionary;

typedef enum kgv_status {
    /* Stable ABI v1 status values. Do not renumber or reuse them. */
    KGV_OK = 0,
    KGV_CANCELLED = 1,
    KGV_INVALID_ARGUMENT = 2,
    KGV_INVALID_TEXT = 3,
    KGV_INVALID_VOICE = 4,
    KGV_INVALID_MODEL = 5,
    KGV_ABI_MISMATCH = 6,
    KGV_BUSY = 7,
    KGV_RESOURCE_EXHAUSTED = 8,
    KGV_INTERNAL_ERROR = 9,
    KGV_HASH_MISMATCH = 10,
    KGV_UNSUPPORTED_SCHEMA = 11,
    KGV_UNSUPPORTED_CPU = 12,
    KGV_IO_ERROR = 13,
    KGV_INPUT_TOO_LARGE = 14,
    KGV_INVALID_STATE = 15
} kgv_status;

typedef enum kgv_text_profile {
    KGV_PROFILE_PROSE = 1,
    KGV_PROFILE_TERMINAL = 2
} kgv_text_profile;

typedef enum kgv_override_kind {
    KGV_OVERRIDE_REPLACEMENT_TEXT = 1,
    KGV_OVERRIDE_PHONE_SYLLABLES = 2
} kgv_override_kind;

/*
 * ABI-extensible records begin with struct_size. A v1 runtime reads the v1
 * prefix and ignores a larger caller-owned trailing extension. Fixed leaf
 * records such as kgv_phone_segment have no struct_size and may not grow.
 */

typedef struct kgv_phone_segment {
    uint16_t segment_id;
    uint8_t syllable_start;
    uint8_t stress; /* 0 none, 1 primary, 2 secondary; nonzero only at a start */
} kgv_phone_segment;

typedef struct kgv_pronunciation_override {
    /* Set struct_size to sizeof(kgv_pronunciation_override). */
    size_t struct_size;
    uint32_t byte_start;
    uint32_t byte_end;
    uint32_t kind;
    const char *replacement_utf8;
    size_t replacement_utf8_size;
    const kgv_phone_segment *phone_segments;
    size_t phone_segment_count;
} kgv_pronunciation_override;

typedef struct kgv_frontend_input {
    /* All source spans address the exact utf8_text bytes supplied here. */
    size_t struct_size;
    const char *utf8_text;
    size_t utf8_size;
    uint32_t profile;
    const kgv_pronunciation_override *overrides;
    size_t override_count;
    const kgv_dictionary *dictionary;
} kgv_frontend_input;

typedef struct kgv_engine_options {
    /* Set struct_size and pass a caller-pinned lowercase RELEASE.json hash. */
    size_t struct_size;
    uint32_t abi_version;
    uint32_t thread_count;
    uint32_t flags;
    const char *expected_release_sha256;
} kgv_engine_options;

typedef struct kgv_request {
    /* Set struct_size. rate is rejected outside 0.70 through 1.50. */
    size_t struct_size;
    kgv_frontend_input frontend;
    const char *voice_id;
    float rate;
    uint64_t seed;
} kgv_request;

typedef int (*kgv_pcm_callback)(const int16_t *mono_frames,
                                size_t frame_count,
                                uint32_t sample_rate,
                                void *user);

KGV_API uint32_t kgv_abi_version(void);
/* The returned static ASCII name is valid for the process lifetime. */
KGV_API const char *kgv_status_name(int status);

/*
 * Opens a local immutable package after verifying the complete caller-pinned
 * RELEASE.json -> MANIFEST.json -> payload chain. The function copies every
 * caller-owned option before returning. On failure, *out_engine is NULL.
 * error may be NULL when error_size is zero; status is the machine contract.
 */
KGV_API int kgv_engine_open(const char *model_directory,
                            const kgv_engine_options *options,
                            kgv_engine **out_engine,
                            char *error,
                            size_t error_size);
/*
 * Validates and copies the request, its UTF-8 bytes, and every override. ABI v1
 * permits only one live job per engine; another creation returns KGV_BUSY.
 */
KGV_API int kgv_job_create(kgv_engine *engine,
                           const kgv_request *request,
                           kgv_job **out_job,
                           char *error,
                           size_t error_size);
/*
 * Blocking call. PCM callbacks are synchronous, serialized, on the calling
 * thread, and contain numeric host int16_t mono frames at 24 kHz. A callback
 * return of zero requests cancellation. The frame pointer is borrowed only
 * for the callback and must not be retained. Do not destroy the job or engine
 * from inside the callback; kgv_job_cancel is safe there and from other threads.
 */
KGV_API int kgv_job_run(kgv_job *job,
                        kgv_pcm_callback callback,
                        void *user,
                        char *error,
                        size_t error_size);
/* Thread-safe, idempotent, and allocation-free. */
KGV_API void kgv_job_cancel(kgv_job *job);
/* Cancels and waits for an in-progress run; never call it from the PCM callback. */
KGV_API void kgv_job_destroy(kgv_job *job);
/* Destroy all jobs before closing their engine. */
KGV_API void kgv_engine_close(kgv_engine *engine);

#ifdef __cplusplus
}
#endif

#endif
