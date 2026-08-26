#include "kilix_voicegen.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct callback_state {
    size_t calls;
    size_t frames;
    size_t cancel_after;
    int invalid_block;
    int saw_nonzero;
} callback_state;

static int consume_pcm(const int16_t *frames, size_t frame_count,
                       uint32_t sample_rate, void *user) {
    callback_state *state = (callback_state *)user;
    size_t index = 0U;
    if (frames == NULL || frame_count == 0U || frame_count > 2400U ||
        sample_rate != KGV_SAMPLE_RATE) {
        state->invalid_block = 1;
        return 0;
    }
    for (index = 0U; index < frame_count; ++index) {
        if (frames[index] != 0) {
            state->saw_nonzero = 1;
        }
    }
    ++state->calls;
    state->frames += frame_count;
    return state->cancel_after == 0U || state->calls < state->cancel_after;
}

static kgv_request request_for(const char *text, size_t text_size,
                               uint32_t profile, const char *voice) {
    kgv_request request;
    memset(&request, 0, sizeof(request));
    request.struct_size = sizeof(request);
    request.frontend.struct_size = sizeof(request.frontend);
    request.frontend.utf8_text = text;
    request.frontend.utf8_size = text_size;
    request.frontend.profile = profile;
    request.voice_id = voice;
    request.rate = 1.0F;
    request.seed = KGV_DEFAULT_SEED;
    return request;
}

static int require_status(int actual, int expected, const char *context,
                          const char *error) {
    if (actual == expected) {
        return 1;
    }
    fprintf(stderr, "%s: expected %s, got %s (%s)\n", context,
            kgv_status_name(expected), kgv_status_name(actual), error == NULL ? "" : error);
    return 0;
}

int main(int argc, char **argv) {
    char error[512];
    kgv_engine_options options;
    kgv_engine *engine = NULL;
    kgv_job *job = NULL;
    kgv_job *second = NULL;
    kgv_request request;
    callback_state callback;
    int status = KGV_OK;
    int code = KGV_OK;

    if (argc != 3) {
        fprintf(stderr, "fixture directory and release SHA are required\n");
        return 2;
    }
    if (kgv_abi_version() != KILIX_VOICEGEN_ABI_VERSION ||
        strcmp(kgv_status_name(9999), "KGV_UNKNOWN_STATUS") != 0) {
        fprintf(stderr, "ABI version or unknown status name is invalid\n");
        return 1;
    }
    for (code = KGV_OK; code <= KGV_INVALID_STATE; ++code) {
        if (strncmp(kgv_status_name(code), "KGV_", 4U) != 0) {
            fprintf(stderr, "stable status %d has no stable name\n", code);
            return 1;
        }
    }

    memset(&options, 0, sizeof(options));
    options.struct_size = sizeof(options);
    options.abi_version = 999U;
    options.thread_count = 1U;
    options.expected_release_sha256 = argv[2];
    status = kgv_engine_open(argv[1], &options, &engine, error, sizeof(error));
    if (!require_status(status, KGV_ABI_MISMATCH, "unknown ABI", error)) {
        return 1;
    }
    options.abi_version = KILIX_VOICEGEN_ABI_VERSION;
    options.struct_size = offsetof(kgv_engine_options, expected_release_sha256);
    status = kgv_engine_open(argv[1], &options, &engine, error, sizeof(error));
    if (!require_status(status, KGV_ABI_MISMATCH, "short engine options", error)) {
        return 1;
    }
    options.struct_size = sizeof(options) + sizeof(uint64_t);
    options.flags = 1U;
    status = kgv_engine_open(argv[1], &options, &engine, error, sizeof(error));
    if (!require_status(status, KGV_ABI_MISMATCH, "unknown flags", error)) {
        return 1;
    }
    options.flags = 0U;
    options.expected_release_sha256 =
        "0000000000000000000000000000000000000000000000000000000000000000";
    status = kgv_engine_open(argv[1], &options, &engine, error, sizeof(error));
    if (!require_status(status, KGV_HASH_MISMATCH, "outer hash", error)) {
        return 1;
    }
    options.expected_release_sha256 = argv[2];
    status = kgv_engine_open(argv[1], &options, &engine, error, sizeof(error));
    if (!require_status(status, KGV_OK, "valid engine", error) || engine == NULL) {
        return 1;
    }

    request = request_for("hello", 5U, KGV_PROFILE_PROSE, "kilix-female-01");
    request.struct_size = offsetof(kgv_request, seed);
    status = kgv_job_create(engine, &request, &job, error, sizeof(error));
    if (!require_status(status, KGV_ABI_MISMATCH, "short request", error)) {
        kgv_engine_close(engine);
        return 1;
    }
    request.struct_size = sizeof(request) + sizeof(uint64_t);
    status = kgv_job_create(engine, &request, &job, error, sizeof(error));
    if (!require_status(status, KGV_OK, "create job", error)) {
        kgv_engine_close(engine);
        return 1;
    }
    status = kgv_job_create(engine, &request, &second, error, sizeof(error));
    if (!require_status(status, KGV_BUSY, "second job", error) || second != NULL) {
        kgv_job_destroy(job);
        kgv_engine_close(engine);
        return 1;
    }
    memset(&callback, 0, sizeof(callback));
    status = kgv_job_run(job, consume_pcm, &callback, error, sizeof(error));
    if (!require_status(status, KGV_OK, "stream job", error) || callback.calls < 2U ||
        callback.frames == 0U || callback.invalid_block || !callback.saw_nonzero) {
        fprintf(stderr, "valid stream did not satisfy callback contract\n");
        kgv_job_destroy(job);
        kgv_engine_close(engine);
        return 1;
    }
    status = kgv_job_run(job, consume_pcm, &callback, error, sizeof(error));
    if (!require_status(status, KGV_INVALID_STATE, "repeat run", error)) {
        kgv_job_destroy(job);
        kgv_engine_close(engine);
        return 1;
    }
    kgv_job_destroy(job);
    job = NULL;

    request = request_for("   \t\n", 5U, KGV_PROFILE_PROSE, "kilix-male-01");
    status = kgv_job_create(engine, &request, &job, error, sizeof(error));
    memset(&callback, 0, sizeof(callback));
    if (!require_status(status, KGV_OK, "whitespace job", error) ||
        !require_status(kgv_job_run(job, consume_pcm, &callback, error, sizeof(error)),
                        KGV_OK, "whitespace run", error) || callback.calls != 0U) {
        fprintf(stderr, "whitespace request emitted PCM\n");
        kgv_job_destroy(job);
        kgv_engine_close(engine);
        return 1;
    }
    kgv_job_destroy(job);
    job = NULL;

    request = request_for("cancel me", 9U, KGV_PROFILE_PROSE, "kilix-male-01");
    status = kgv_job_create(engine, &request, &job, error, sizeof(error));
    memset(&callback, 0, sizeof(callback));
    callback.cancel_after = 2U;
    if (!require_status(status, KGV_OK, "cancel job", error) ||
        !require_status(kgv_job_run(job, consume_pcm, &callback, error, sizeof(error)),
                        KGV_CANCELLED, "callback cancellation", error) ||
        callback.calls != 2U) {
        kgv_job_destroy(job);
        kgv_engine_close(engine);
        return 1;
    }
    kgv_job_destroy(job);
    job = NULL;

    {
        char mutable_text[] = "copied";
        request = request_for(mutable_text, 6U, KGV_PROFILE_PROSE, "kilix-female-01");
        status = kgv_job_create(engine, &request, &job, error, sizeof(error));
        memset(mutable_text, ' ', 6U);
        memset(&callback, 0, sizeof(callback));
        if (!require_status(status, KGV_OK, "copied request", error) ||
            !require_status(kgv_job_run(job, consume_pcm, &callback, error, sizeof(error)),
                            KGV_OK, "copied run", error) || callback.calls == 0U) {
            fprintf(stderr, "job did not retain a copy of caller input\n");
            kgv_job_destroy(job);
            kgv_engine_close(engine);
            return 1;
        }
        kgv_job_destroy(job);
        job = NULL;
    }

    request = request_for("\x1b[31merror\x1b[0m", 14U, KGV_PROFILE_TERMINAL,
                          "kilix-female-01");
    status = kgv_job_create(engine, &request, &job, error, sizeof(error));
    if (!require_status(status, KGV_OK, "terminal ANSI", error)) {
        kgv_engine_close(engine);
        return 1;
    }
    kgv_job_destroy(job);
    job = NULL;
    request.frontend.profile = KGV_PROFILE_PROSE;
    status = kgv_job_create(engine, &request, &job, error, sizeof(error));
    if (!require_status(status, KGV_INVALID_TEXT, "prose ANSI", error)) {
        kgv_engine_close(engine);
        return 1;
    }

    {
        const char malformed[] = {(char)0xc3, (char)0x28};
        request = request_for(malformed, sizeof(malformed), KGV_PROFILE_PROSE,
                              "kilix-female-01");
        status = kgv_job_create(engine, &request, &job, error, sizeof(error));
        if (!require_status(status, KGV_INVALID_TEXT, "malformed UTF-8", error)) {
            kgv_engine_close(engine);
            return 1;
        }
    }
    {
        const char with_nul[] = {'a', '\0', 'b'};
        request = request_for(with_nul, sizeof(with_nul), KGV_PROFILE_PROSE,
                              "kilix-female-01");
        status = kgv_job_create(engine, &request, &job, error, sizeof(error));
        if (!require_status(status, KGV_INVALID_TEXT, "embedded NUL", error)) {
            kgv_engine_close(engine);
            return 1;
        }
    }
    request = request_for("hello", 5U, 0U, "kilix-female-01");
    status = kgv_job_create(engine, &request, &job, error, sizeof(error));
    if (!require_status(status, KGV_INVALID_ARGUMENT, "profile", error)) {
        kgv_engine_close(engine);
        return 1;
    }
    request = request_for("hello", 5U, KGV_PROFILE_PROSE, "not-a-voice");
    status = kgv_job_create(engine, &request, &job, error, sizeof(error));
    if (!require_status(status, KGV_INVALID_VOICE, "voice", error)) {
        kgv_engine_close(engine);
        return 1;
    }
    request = request_for("hello", 5U, KGV_PROFILE_PROSE, "kilix-female-01");
    request.rate = NAN;
    status = kgv_job_create(engine, &request, &job, error, sizeof(error));
    if (!require_status(status, KGV_INVALID_ARGUMENT, "rate", error)) {
        kgv_engine_close(engine);
        return 1;
    }

    {
        char *large = (char *)malloc((size_t)KGV_MAX_INPUT_BYTES + 1U);
        if (large == NULL) {
            fprintf(stderr, "could not allocate input-limit fixture\n");
            kgv_engine_close(engine);
            return 1;
        }
        memset(large, 'a', (size_t)KGV_MAX_INPUT_BYTES + 1U);
        request = request_for(large, (size_t)KGV_MAX_INPUT_BYTES + 1U,
                              KGV_PROFILE_PROSE, "kilix-female-01");
        status = kgv_job_create(engine, &request, &job, error, sizeof(error));
        free(large);
        if (!require_status(status, KGV_INPUT_TOO_LARGE, "input limit", error)) {
            kgv_engine_close(engine);
            return 1;
        }
    }

    {
        kgv_pronunciation_override override;
        kgv_phone_segment segment;
        memset(&override, 0, sizeof(override));
        segment.segment_id = 4U;
        segment.syllable_start = 1U;
        segment.stress = 1U;
        override.struct_size = sizeof(override);
        override.byte_start = 0U;
        override.byte_end = 5U;
        override.kind = KGV_OVERRIDE_PHONE_SYLLABLES;
        override.phone_segments = &segment;
        override.phone_segment_count = 1U;
        request = request_for("Kilix", 5U, KGV_PROFILE_PROSE, "kilix-female-01");
        request.frontend.overrides = &override;
        request.frontend.override_count = 1U;
        status = kgv_job_create(engine, &request, &job, error, sizeof(error));
        if (!require_status(status, KGV_OK, "typed phone override", error)) {
            kgv_engine_close(engine);
            return 1;
        }
        kgv_job_destroy(job);
        job = NULL;
        segment.segment_id = 999U;
        status = kgv_job_create(engine, &request, &job, error, sizeof(error));
        if (!require_status(status, KGV_INVALID_TEXT, "unknown phone", error)) {
            kgv_engine_close(engine);
            return 1;
        }
    }

    kgv_engine_close(engine);
    return 0;
}
