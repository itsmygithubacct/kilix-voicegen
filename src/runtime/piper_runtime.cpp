#include "runtime/piper_runtime.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(KGV_ENABLE_ONNXRUNTIME)
#include <dlfcn.h>

#include "onnxruntime/core/session/onnxruntime_c_api.h"
#endif

namespace kgv {
namespace {

constexpr std::size_t kCallbackFrames = 480U;
constexpr std::size_t kMaximumNativeSamples = 22050U * 300U;
constexpr std::size_t kResamplerRadius = 16U;
constexpr double kPi = 3.141592653589793238462643383279502884;

int reject_runtime(std::unique_ptr<PiperRuntime> *runtime,
                   std::string *error,
                   int status,
                   std::string message) {
    if (runtime != nullptr) {
        runtime->reset();
    }
    if (error != nullptr) {
        *error = std::move(message);
    }
    return status;
}

double sinc(double value) noexcept {
    if (std::abs(value) < 1.0e-12) {
        return 1.0;
    }
    const double radians = kPi * value;
    return std::sin(radians) / radians;
}

[[maybe_unused]] int resample_and_emit(const std::vector<float> &native,
                      std::uint32_t native_rate,
                      std::atomic<bool> *cancelled,
                      kgv_pcm_callback callback,
                      void *user,
                      std::string *error) {
    if (native.empty() || native_rate == 0U || cancelled == nullptr ||
        callback == nullptr || error == nullptr) {
        *error = "Piper produced an invalid native audio buffer";
        return KGV_INTERNAL_ERROR;
    }

    for (float sample : native) {
        if (!std::isfinite(sample)) {
            *error = "Piper produced a non-finite audio sample";
            return KGV_INTERNAL_ERROR;
        }
    }
    if (native.size() >
        (std::numeric_limits<std::size_t>::max() - KGV_SAMPLE_RATE / 2U) /
            KGV_SAMPLE_RATE) {
        *error = "Piper resampled output size overflowed";
        return KGV_RESOURCE_EXHAUSTED;
    }
    const std::size_t output_count =
        (native.size() * static_cast<std::size_t>(KGV_SAMPLE_RATE) +
         static_cast<std::size_t>(native_rate / 2U)) /
        static_cast<std::size_t>(native_rate);
    if (output_count == 0U || output_count > KGV_SAMPLE_RATE * 300U) {
        *error = "Piper resampled output exceeds the five-minute chunk limit";
        return KGV_RESOURCE_EXHAUSTED;
    }

    std::vector<std::int16_t> block;
    block.reserve(kCallbackFrames);
    for (std::size_t output_index = 0U; output_index < output_count;
         ++output_index) {
        if ((output_index & 4095U) == 0U &&
            cancelled->load(std::memory_order_acquire)) {
            *error = "synthesis was cancelled";
            return KGV_CANCELLED;
        }
        const double position =
            static_cast<double>(output_index) * static_cast<double>(native_rate) /
            static_cast<double>(KGV_SAMPLE_RATE);
        const auto centre = static_cast<std::int64_t>(std::floor(position));
        double weighted = 0.0;
        double weight_sum = 0.0;
        const auto radius = static_cast<std::int64_t>(kResamplerRadius);
        for (std::int64_t input_index = centre - radius + 1;
             input_index <= centre + radius; ++input_index) {
            if (input_index < 0 ||
                static_cast<std::uint64_t>(input_index) >= native.size()) {
                continue;
            }
            const double distance = position - static_cast<double>(input_index);
            if (std::abs(distance) >= static_cast<double>(kResamplerRadius)) {
                continue;
            }
            const double weight = sinc(distance) *
                                  sinc(distance /
                                       static_cast<double>(kResamplerRadius));
            weighted += static_cast<double>(
                            native[static_cast<std::size_t>(input_index)]) *
                        weight;
            weight_sum += weight;
        }
        double sample = weight_sum == 0.0 ? 0.0 : weighted / weight_sum;
        sample = std::max(-1.0, std::min(1.0, sample));
        const long encoded = std::lround(sample * 32767.0);
        block.push_back(static_cast<std::int16_t>(encoded));
        if (block.size() == kCallbackFrames || output_index + 1U == output_count) {
            if (cancelled->load(std::memory_order_acquire)) {
                *error = "synthesis was cancelled";
                return KGV_CANCELLED;
            }
            if (callback(block.data(), block.size(), KGV_SAMPLE_RATE, user) == 0) {
                cancelled->store(true, std::memory_order_release);
                *error = "synthesis was cancelled by the PCM consumer";
                return KGV_CANCELLED;
            }
            block.clear();
        }
    }
    return KGV_OK;
}

}  // namespace

#if defined(KGV_ENABLE_ONNXRUNTIME)

namespace {

char kRuntimeModuleAnchor = 0;

using GetApiBaseFunction = const OrtApiBase *(ORT_API_CALL *)(void);

std::string bounded_ort_message(const OrtApi *api,
                                const OrtStatus *status,
                                std::string_view context) {
    std::string result(context);
    result += ": ";
    const char *message = api->GetErrorMessage(status);
    if (message == nullptr) {
        result += "ONNX Runtime returned an error without detail";
    } else {
        const std::size_t size = std::min<std::size_t>(std::strlen(message), 512U);
        result.append(message, size);
    }
    return result;
}

bool ort_call(const OrtApi *api,
              OrtStatus *status,
              std::string_view context,
              std::string *error) {
    if (status == nullptr) {
        return true;
    }
    *error = bounded_ort_message(api, status, context);
    api->ReleaseStatus(status);
    return false;
}

void *open_ort_library(std::string *error) {
    Dl_info module{};
    if (dladdr(static_cast<void *>(&kRuntimeModuleAnchor), &module) == 0 ||
        module.dli_fname == nullptr) {
        *error = "could not locate the Kilix Voicegen runtime library";
        return nullptr;
    }
    std::string sibling(module.dli_fname);
    const std::size_t slash = sibling.find_last_of('/');
    if (slash == std::string::npos) {
        *error = "Kilix Voicegen runtime library path has no sibling directory";
        return nullptr;
    }
    sibling.resize(slash + 1U);
    sibling += KGV_ONNXRUNTIME_LIBRARY_NAME;
    dlerror();
    if (void *handle = dlopen(sibling.c_str(), RTLD_NOW | RTLD_LOCAL)) {
        return handle;
    }
    const char *detail = dlerror();
    *error = "could not load the pinned ONNX Runtime shared library";
    if (detail != nullptr) {
        *error += ": ";
        error->append(detail, std::min<std::size_t>(std::strlen(detail), 512U));
    }
    return nullptr;
}

GetApiBaseFunction load_api_base(void *library, std::string *error) {
    dlerror();
    void *symbol = dlsym(library, "OrtGetApiBase");
    const char *detail = dlerror();
    if (symbol == nullptr || detail != nullptr) {
        *error = "pinned ONNX Runtime lacks OrtGetApiBase";
        return nullptr;
    }
    GetApiBaseFunction function = nullptr;
    static_assert(sizeof(function) == sizeof(symbol),
                  "POSIX function and object pointers must have equal size");
    std::memcpy(&function, &symbol, sizeof(function));
    return function;
}

struct ExpectedTensor final {
    std::string_view name;
    ONNXTensorElementDataType type;
    std::vector<std::int64_t> dimensions;
};

bool compatible_dimension(std::int64_t actual, std::int64_t expected) noexcept {
    return expected < 0 ? actual < 0 : actual == expected;
}

bool inspect_tensor(const OrtApi *api,
                    const OrtSession *session,
                    OrtAllocator *allocator,
                    std::size_t index,
                    bool input,
                    const std::vector<ExpectedTensor> &expected,
                    std::set<std::string, std::less<>> *seen,
                    std::string *error) {
    char *raw_name = nullptr;
    OrtStatus *name_status = input
                                 ? api->SessionGetInputName(session, index, allocator,
                                                            &raw_name)
                                 : api->SessionGetOutputName(session, index, allocator,
                                                             &raw_name);
    if (!ort_call(api, name_status, "could not inspect graph tensor name", error)) {
        return false;
    }
    std::string name = raw_name == nullptr ? std::string{} : std::string(raw_name);
    if (raw_name != nullptr) {
        allocator->Free(allocator, raw_name);
    }
    const auto contract = std::find_if(
        expected.begin(), expected.end(), [&name](const ExpectedTensor &entry) {
            return entry.name == name;
        });
    if (contract == expected.end() || !seen->insert(name).second) {
        *error = "ONNX graph contains an unexpected or duplicate tensor name";
        return false;
    }

    OrtTypeInfo *type_info = nullptr;
    OrtStatus *type_status = input
                                 ? api->SessionGetInputTypeInfo(session, index, &type_info)
                                 : api->SessionGetOutputTypeInfo(session, index, &type_info);
    if (!ort_call(api, type_status, "could not inspect graph tensor type", error)) {
        return false;
    }
    const OrtTensorTypeAndShapeInfo *tensor_info = nullptr;
    if (!ort_call(api, api->CastTypeInfoToTensorInfo(type_info, &tensor_info),
                  "graph value is not a tensor", error)) {
        api->ReleaseTypeInfo(type_info);
        return false;
    }
    ONNXTensorElementDataType element_type =
        ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    std::size_t rank = 0U;
    bool valid = ort_call(api, api->GetTensorElementType(tensor_info, &element_type),
                          "could not inspect graph tensor element type", error) &&
                 ort_call(api, api->GetDimensionsCount(tensor_info, &rank),
                          "could not inspect graph tensor rank", error);
    std::vector<std::int64_t> dimensions(rank);
    if (valid && rank > 0U) {
        valid = ort_call(api,
                         api->GetDimensions(tensor_info, dimensions.data(), rank),
                         "could not inspect graph tensor dimensions", error);
    }
    api->ReleaseTypeInfo(type_info);
    if (!valid) {
        return false;
    }
    if (element_type != contract->type ||
        dimensions.size() != contract->dimensions.size()) {
        *error = "ONNX graph tensor type or rank does not match its manifest";
        return false;
    }
    for (std::size_t dimension = 0U; dimension < dimensions.size(); ++dimension) {
        if (!compatible_dimension(dimensions[dimension],
                                  contract->dimensions[dimension])) {
            *error = "ONNX graph tensor dimensions do not match its manifest";
            return false;
        }
    }
    return true;
}

bool validate_graph_contract(const OrtApi *api,
                             const OrtSession *session,
                             std::string *error) {
    std::size_t input_count = 0U;
    std::size_t output_count = 0U;
    if (!ort_call(api, api->SessionGetInputCount(session, &input_count),
                  "could not inspect graph inputs", error) ||
        !ort_call(api, api->SessionGetOutputCount(session, &output_count),
                  "could not inspect graph outputs", error)) {
        return false;
    }
    if (input_count != 3U || output_count != 1U) {
        *error = "ONNX graph input/output count does not match Piper v1";
        return false;
    }
    OrtAllocator *allocator = nullptr;
    if (!ort_call(api, api->GetAllocatorWithDefaultOptions(&allocator),
                  "could not obtain the ONNX Runtime allocator", error)) {
        return false;
    }
    const std::vector<ExpectedTensor> inputs = {
        {"input", ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, {-1, -1}},
        {"input_lengths", ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, {-1}},
        {"scales", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {3}},
    };
    const std::vector<ExpectedTensor> outputs = {
        {"output", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {-1, 1, 1, -1}},
    };
    std::set<std::string, std::less<>> seen_inputs;
    std::set<std::string, std::less<>> seen_outputs;
    for (std::size_t index = 0U; index < input_count; ++index) {
        if (!inspect_tensor(api, session, allocator, index, true, inputs,
                            &seen_inputs, error)) {
            return false;
        }
    }
    for (std::size_t index = 0U; index < output_count; ++index) {
        if (!inspect_tensor(api, session, allocator, index, false, outputs,
                            &seen_outputs, error)) {
            return false;
        }
    }
    return seen_inputs.size() == inputs.size() &&
           seen_outputs.size() == outputs.size();
}

}  // namespace

struct PiperRuntime::Impl final {
    void *library = nullptr;
    const OrtApi *api = nullptr;
    OrtEnv *environment = nullptr;
    OrtSession *session = nullptr;
    OrtMemoryInfo *memory_info = nullptr;
    std::uint32_t model_sample_rate = 0U;
    float noise_scale = 0.0F;
    float noise_w = 0.0F;
    std::mutex active_mutex;
    OrtRunOptions *active_run_options = nullptr;

    ~Impl() {
        if (api != nullptr) {
            if (memory_info != nullptr) api->ReleaseMemoryInfo(memory_info);
            if (session != nullptr) api->ReleaseSession(session);
            if (environment != nullptr) api->ReleaseEnv(environment);
        }
        if (library != nullptr) {
            (void)dlclose(library);
        }
    }

    int run_chunk(const std::vector<std::int64_t> &ids,
                  float rate,
                  std::atomic<bool> *cancelled,
                  std::vector<float> *audio,
                  std::string *error) {
        audio->clear();
        if (ids.empty() || ids.size() > 8192U || !std::isfinite(rate) ||
            rate <= 0.0F) {
            *error = "Piper received an invalid projected token chunk";
            return KGV_INVALID_ARGUMENT;
        }
        std::int64_t input_length = static_cast<std::int64_t>(ids.size());
        std::vector<std::int64_t> mutable_ids(ids);
        float scales[3] = {noise_scale, 1.0F / rate, noise_w};
        const std::int64_t input_shape[2] = {1, input_length};
        const std::int64_t vector_shape[1] = {1};
        const std::int64_t scales_shape[1] = {3};
        OrtValue *values[3] = {nullptr, nullptr, nullptr};
        const auto release_values = [&]() {
            for (OrtValue *value : values) {
                if (value != nullptr) api->ReleaseValue(value);
            }
        };
        if (!ort_call(api,
                      api->CreateTensorWithDataAsOrtValue(
                          memory_info, mutable_ids.data(),
                          mutable_ids.size() * sizeof(std::int64_t), input_shape, 2U,
                          ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &values[0]),
                      "could not create Piper input tensor", error) ||
            !ort_call(api,
                      api->CreateTensorWithDataAsOrtValue(
                          memory_info, &input_length, sizeof(input_length), vector_shape,
                          1U, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &values[1]),
                      "could not create Piper length tensor", error) ||
            !ort_call(api,
                      api->CreateTensorWithDataAsOrtValue(
                          memory_info, scales, sizeof(scales), scales_shape, 1U,
                          ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &values[2]),
                      "could not create Piper scales tensor", error)) {
            release_values();
            return KGV_INTERNAL_ERROR;
        }

        OrtRunOptions *run_options = nullptr;
        if (!ort_call(api, api->CreateRunOptions(&run_options),
                      "could not create Piper run options", error)) {
            release_values();
            return KGV_INTERNAL_ERROR;
        }
        {
            std::lock_guard<std::mutex> lock(active_mutex);
            active_run_options = run_options;
            if (cancelled->load(std::memory_order_acquire)) {
                OrtStatus *status = api->RunOptionsSetTerminate(run_options);
                if (status != nullptr) api->ReleaseStatus(status);
            }
        }

        static constexpr const char *kInputNames[] = {
            "input", "input_lengths", "scales",
        };
        static constexpr const char *kOutputNames[] = {"output"};
        const OrtValue *const_inputs[] = {values[0], values[1], values[2]};
        OrtValue *output = nullptr;
        OrtStatus *run_status = api->Run(session, run_options, kInputNames,
                                         const_inputs, 3U, kOutputNames, 1U, &output);
        {
            std::lock_guard<std::mutex> lock(active_mutex);
            active_run_options = nullptr;
        }
        api->ReleaseRunOptions(run_options);
        release_values();
        if (run_status != nullptr) {
            const std::string detail = bounded_ort_message(
                api, run_status, "Piper ONNX inference failed");
            api->ReleaseStatus(run_status);
            if (output != nullptr) api->ReleaseValue(output);
            if (cancelled->load(std::memory_order_acquire)) {
                *error = "synthesis was cancelled during ONNX inference";
                return KGV_CANCELLED;
            }
            *error = detail;
            return KGV_INTERNAL_ERROR;
        }
        if (output == nullptr) {
            *error = "Piper ONNX inference returned no output tensor";
            return KGV_INTERNAL_ERROR;
        }

        OrtTensorTypeAndShapeInfo *shape = nullptr;
        if (!ort_call(api, api->GetTensorTypeAndShape(output, &shape),
                      "could not inspect Piper output shape", error)) {
            api->ReleaseValue(output);
            return KGV_INTERNAL_ERROR;
        }
        ONNXTensorElementDataType element_type =
            ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
        std::size_t rank = 0U;
        std::size_t element_count = 0U;
        bool valid = ort_call(api, api->GetTensorElementType(shape, &element_type),
                              "could not inspect Piper output type", error) &&
                     ort_call(api, api->GetDimensionsCount(shape, &rank),
                              "could not inspect Piper output rank", error);
        std::vector<std::int64_t> dimensions(rank);
        if (valid && rank > 0U) {
            valid = ort_call(api, api->GetDimensions(shape, dimensions.data(), rank),
                             "could not inspect Piper output dimensions", error) &&
                    ort_call(api, api->GetTensorShapeElementCount(shape,
                                                                  &element_count),
                             "could not inspect Piper output size", error);
        } else if (valid) {
            valid = ort_call(api, api->GetTensorShapeElementCount(shape,
                                                                  &element_count),
                             "could not inspect Piper output size", error);
        }
        api->ReleaseTensorTypeAndShapeInfo(shape);
        if (!valid || element_type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
            dimensions.size() != 4U || dimensions[0] != 1 ||
            dimensions[1] != 1 || dimensions[2] != 1 || dimensions[3] <= 0 ||
            element_count != static_cast<std::size_t>(dimensions[3]) ||
            element_count > kMaximumNativeSamples) {
            api->ReleaseValue(output);
            if (valid) {
                *error = "Piper output tensor violates the verified audio contract";
            }
            return valid ? KGV_INVALID_MODEL : KGV_INTERNAL_ERROR;
        }
        void *raw_audio = nullptr;
        if (!ort_call(api, api->GetTensorMutableData(output, &raw_audio),
                      "could not read Piper output tensor", error) ||
            raw_audio == nullptr) {
            api->ReleaseValue(output);
            return KGV_INTERNAL_ERROR;
        }
        const auto *samples = static_cast<const float *>(raw_audio);
        audio->assign(samples, samples + element_count);
        api->ReleaseValue(output);
        return KGV_OK;
    }
};

PiperRuntime::PiperRuntime(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

PiperRuntime::~PiperRuntime() = default;

int PiperRuntime::create(const VerifiedModel &model,
                         std::uint32_t thread_count,
                         std::unique_ptr<PiperRuntime> *runtime,
                         std::string *error) {
    if (runtime == nullptr || error == nullptr || thread_count == 0U) {
        return KGV_INVALID_ARGUMENT;
    }
    runtime->reset();
    error->clear();
    if (model.engine_kind != "piper-vits-onnx/v1" ||
        model.model_sample_rate != 22050U || model.target_id_max != 255U) {
        return reject_runtime(runtime, error, KGV_INVALID_MODEL,
                              "verified model is not a supported Piper v1 package");
    }
    const VerifiedPayload *onnx = model.payload_for_role("onnx_model");
    if (onnx == nullptr || onnx->bytes.empty()) {
        return reject_runtime(runtime, error, KGV_INVALID_MODEL,
                              "verified Piper package lacks ONNX model bytes");
    }

    try {
        auto implementation = std::make_unique<Impl>();
        implementation->library = open_ort_library(error);
        if (implementation->library == nullptr) {
            return reject_runtime(runtime, error, KGV_IO_ERROR, *error);
        }
        const GetApiBaseFunction get_api_base =
            load_api_base(implementation->library, error);
        if (get_api_base == nullptr) {
            return reject_runtime(runtime, error, KGV_IO_ERROR, *error);
        }
        const OrtApiBase *base = get_api_base();
        if (base == nullptr || base->GetVersionString == nullptr ||
            std::string_view(base->GetVersionString()) != KGV_ONNXRUNTIME_VERSION) {
            return reject_runtime(runtime, error, KGV_ABI_MISMATCH,
                                  "loaded ONNX Runtime version does not match the pinned build");
        }
        implementation->api = base->GetApi(ORT_API_VERSION);
        if (implementation->api == nullptr) {
            return reject_runtime(runtime, error, KGV_ABI_MISMATCH,
                                  "loaded ONNX Runtime does not implement the compiled C API");
        }
        const OrtApi *api = implementation->api;
        if (!ort_call(api,
                      api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "kilix-voicegen",
                                     &implementation->environment),
                      "could not create ONNX Runtime environment", error)) {
            return reject_runtime(runtime, error, KGV_INTERNAL_ERROR, *error);
        }
        if (!ort_call(api, api->DisableTelemetryEvents(implementation->environment),
                      "could not disable ONNX Runtime telemetry", error)) {
            return reject_runtime(runtime, error, KGV_INTERNAL_ERROR, *error);
        }

        OrtSessionOptions *options = nullptr;
        if (!ort_call(api, api->CreateSessionOptions(&options),
                      "could not create ONNX session options", error)) {
            return reject_runtime(runtime, error, KGV_INTERNAL_ERROR, *error);
        }
        bool options_ok =
            ort_call(api, api->SetSessionExecutionMode(options, ORT_SEQUENTIAL),
                     "could not select sequential ONNX execution", error) &&
            ort_call(api,
                     api->SetSessionGraphOptimizationLevel(options,
                                                           ORT_ENABLE_ALL),
                     "could not enable ONNX graph optimization", error) &&
            ort_call(api,
                     api->SetIntraOpNumThreads(options,
                                               static_cast<int>(thread_count)),
                     "could not set ONNX intra-op threads", error) &&
            ort_call(api, api->SetInterOpNumThreads(options, 1),
                     "could not set ONNX inter-op threads", error);
        if (options_ok) {
            options_ok = ort_call(
                api,
                api->CreateSessionFromArray(
                    implementation->environment, onnx->bytes.data(), onnx->bytes.size(),
                    options, &implementation->session),
                "could not load the verified Piper ONNX graph", error);
        }
        api->ReleaseSessionOptions(options);
        if (!options_ok) {
            return reject_runtime(runtime, error, KGV_INVALID_MODEL, *error);
        }
        if (!validate_graph_contract(api, implementation->session, error)) {
            return reject_runtime(runtime, error, KGV_INVALID_MODEL, *error);
        }
        if (!ort_call(api,
                      api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault,
                                               &implementation->memory_info),
                      "could not create ONNX CPU memory descriptor", error)) {
            return reject_runtime(runtime, error, KGV_INTERNAL_ERROR, *error);
        }
        implementation->model_sample_rate = model.model_sample_rate;
        implementation->noise_scale =
            static_cast<float>(model.noise_scale_milli) / 1000.0F;
        implementation->noise_w =
            static_cast<float>(model.noise_w_milli) / 1000.0F;
        runtime->reset(new PiperRuntime(std::move(implementation)));
        return KGV_OK;
    } catch (const std::bad_alloc &) {
        return reject_runtime(runtime, error, KGV_RESOURCE_EXHAUSTED,
                              "Piper runtime initialization ran out of memory");
    } catch (const std::exception &) {
        return reject_runtime(runtime, error, KGV_INTERNAL_ERROR,
                              "unexpected Piper runtime initialization failure");
    }
}

int PiperRuntime::synthesize(const std::vector<PiperProjectedChunk> &chunks,
                             float rate,
                             std::atomic<bool> *cancelled,
                             kgv_pcm_callback callback,
                             void *user,
                             std::string *error) {
    if (implementation_ == nullptr || cancelled == nullptr || callback == nullptr ||
        error == nullptr) {
        return KGV_INVALID_ARGUMENT;
    }
    error->clear();
    try {
        std::vector<float> native;
        for (const PiperProjectedChunk &chunk : chunks) {
            if (cancelled->load(std::memory_order_acquire)) {
                *error = "synthesis was cancelled";
                return KGV_CANCELLED;
            }
            int status = implementation_->run_chunk(chunk.ids, rate, cancelled,
                                                    &native, error);
            if (status != KGV_OK) {
                return status;
            }
            status = resample_and_emit(native, implementation_->model_sample_rate,
                                       cancelled, callback, user, error);
            if (status != KGV_OK) {
                return status;
            }
        }
        return KGV_OK;
    } catch (const std::bad_alloc &) {
        *error = "Piper synthesis ran out of memory";
        return KGV_RESOURCE_EXHAUSTED;
    } catch (const std::exception &) {
        *error = "unexpected Piper synthesis failure";
        return KGV_INTERNAL_ERROR;
    }
}

void PiperRuntime::cancel_active() noexcept {
    if (implementation_ == nullptr || implementation_->api == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(implementation_->active_mutex);
    if (implementation_->active_run_options != nullptr) {
        OrtStatus *status = implementation_->api->RunOptionsSetTerminate(
            implementation_->active_run_options);
        if (status != nullptr) implementation_->api->ReleaseStatus(status);
    }
}

#else

struct PiperRuntime::Impl final {};

PiperRuntime::PiperRuntime(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

PiperRuntime::~PiperRuntime() = default;

int PiperRuntime::create(const VerifiedModel &,
                         std::uint32_t,
                         std::unique_ptr<PiperRuntime> *runtime,
                         std::string *error) {
    return reject_runtime(runtime, error, KGV_UNSUPPORTED_SCHEMA,
                          "runtime was built without pinned ONNX Runtime support");
}

int PiperRuntime::synthesize(const std::vector<PiperProjectedChunk> &,
                             float,
                             std::atomic<bool> *,
                             kgv_pcm_callback,
                             void *,
                             std::string *error) {
    if (error != nullptr) {
        *error = "runtime was built without pinned ONNX Runtime support";
    }
    return KGV_UNSUPPORTED_SCHEMA;
}

void PiperRuntime::cancel_active() noexcept {}

#endif

}  // namespace kgv
