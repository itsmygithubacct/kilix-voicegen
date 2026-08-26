#include "kilix_voicegen.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "frontend/pipeline.h"

namespace {

struct Arguments final {
    std::string model;
    std::string release_sha;
    std::string voice;
    std::string profile;
    std::string text;
    std::string output;
    float rate = 1.0F;
    std::uint64_t seed = KGV_DEFAULT_SEED;
    bool read_stdin = false;
};

void usage(std::ostream &stream) {
    stream <<
        "Usage:\n"
        "  kilix-voicegen --version\n"
        "  kilix-voicegen --abi-version\n"
        "  kilix-voicegen verify --model DIR --release-sha HEX\n"
        "  kilix-voicegen frontend --profile prose|terminal (--stdin | --text UTF8)\n"
        "  kilix-voicegen synthesize --model DIR --release-sha HEX\n"
        "      --voice kilix-female-01|kilix-male-01 --profile prose|terminal\n"
        "      (--stdin | --text UTF8) --output FILE.wav [--rate 0.70..1.50]\n"
        "      [--seed UINT64]\n";
}

bool parse_float(std::string_view text, float *result) {
    const auto converted = std::from_chars(text.data(), text.data() + text.size(), *result,
                                           std::chars_format::general);
    return converted.ec == std::errc{} && converted.ptr == text.data() + text.size();
}

bool parse_u64(std::string_view text, std::uint64_t *result) {
    const auto converted = std::from_chars(text.data(), text.data() + text.size(), *result, 10);
    return converted.ec == std::errc{} && converted.ptr == text.data() + text.size();
}

bool take_value(int argc, char **argv, int *index, std::string *value) {
    if (*index + 1 >= argc) {
        return false;
    }
    ++*index;
    *value = argv[*index];
    return true;
}

bool parse_arguments(int argc, char **argv, int start, Arguments *arguments,
                     std::string *error) {
    for (int index = start; index < argc; ++index) {
        const std::string_view option(argv[index]);
        if (option == "--model") {
            if (!take_value(argc, argv, &index, &arguments->model)) {
                *error = "--model requires a value";
                return false;
            }
        } else if (option == "--release-sha") {
            if (!take_value(argc, argv, &index, &arguments->release_sha)) {
                *error = "--release-sha requires a value";
                return false;
            }
        } else if (option == "--voice") {
            if (!take_value(argc, argv, &index, &arguments->voice)) {
                *error = "--voice requires a value";
                return false;
            }
        } else if (option == "--profile") {
            if (!take_value(argc, argv, &index, &arguments->profile)) {
                *error = "--profile requires a value";
                return false;
            }
        } else if (option == "--text") {
            if (!take_value(argc, argv, &index, &arguments->text)) {
                *error = "--text requires a value";
                return false;
            }
        } else if (option == "--output") {
            if (!take_value(argc, argv, &index, &arguments->output)) {
                *error = "--output requires a value";
                return false;
            }
        } else if (option == "--rate") {
            std::string value;
            if (!take_value(argc, argv, &index, &value) ||
                !parse_float(value, &arguments->rate)) {
                *error = "--rate requires a decimal number";
                return false;
            }
        } else if (option == "--seed") {
            std::string value;
            if (!take_value(argc, argv, &index, &value) ||
                !parse_u64(value, &arguments->seed)) {
                *error = "--seed requires an unsigned decimal integer";
                return false;
            }
        } else if (option == "--stdin") {
            arguments->read_stdin = true;
        } else {
            *error = "unknown command-line option";
            return false;
        }
    }
    return true;
}

bool read_bounded_stdin(std::string *text, std::string *error) {
    std::array<char, 4096> buffer{};
    while (std::cin) {
        std::cin.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = std::cin.gcount();
        if (count > 0) {
            const auto amount = static_cast<std::size_t>(count);
            if (text->size() > KGV_MAX_INPUT_BYTES ||
                amount > KGV_MAX_INPUT_BYTES - text->size()) {
                *error = "standard input exceeds the 65536-byte runtime limit";
                return false;
            }
            text->append(buffer.data(), amount);
        }
    }
    if (!std::cin.eof()) {
        *error = "could not read standard input";
        return false;
    }
    return true;
}

void append_u16_le(std::array<unsigned char, 44> *header, std::size_t offset,
                   std::uint16_t value) {
    (*header)[offset] = static_cast<unsigned char>(value & 0xffU);
    (*header)[offset + 1U] = static_cast<unsigned char>(value >> 8U);
}

void append_u32_le(std::array<unsigned char, 44> *header, std::size_t offset,
                   std::uint32_t value) {
    (*header)[offset] = static_cast<unsigned char>(value & 0xffU);
    (*header)[offset + 1U] = static_cast<unsigned char>((value >> 8U) & 0xffU);
    (*header)[offset + 2U] = static_cast<unsigned char>((value >> 16U) & 0xffU);
    (*header)[offset + 3U] = static_cast<unsigned char>(value >> 24U);
}

std::array<unsigned char, 44> wav_header(std::uint32_t data_bytes) {
    std::array<unsigned char, 44> header{};
    const std::array<unsigned char, 4> riff = {'R', 'I', 'F', 'F'};
    const std::array<unsigned char, 4> wave = {'W', 'A', 'V', 'E'};
    const std::array<unsigned char, 4> fmt = {'f', 'm', 't', ' '};
    const std::array<unsigned char, 4> data = {'d', 'a', 't', 'a'};
    std::copy(riff.begin(), riff.end(), header.begin());
    std::copy(wave.begin(), wave.end(), header.begin() + 8);
    std::copy(fmt.begin(), fmt.end(), header.begin() + 12);
    std::copy(data.begin(), data.end(), header.begin() + 36);
    append_u32_le(&header, 4U, 36U + data_bytes);
    append_u32_le(&header, 16U, 16U);
    append_u16_le(&header, 20U, 1U);
    append_u16_le(&header, 22U, 1U);
    append_u32_le(&header, 24U, KGV_SAMPLE_RATE);
    append_u32_le(&header, 28U, KGV_SAMPLE_RATE * 2U);
    append_u16_le(&header, 32U, 2U);
    append_u16_le(&header, 34U, 16U);
    append_u32_le(&header, 40U, data_bytes);
    return header;
}

struct WavSink final {
    std::ofstream stream;
    std::uint64_t frames = 0U;
    bool failed = false;
};

int write_pcm(const std::int16_t *frames, std::size_t frame_count,
              std::uint32_t sample_rate, void *user) {
    auto *sink = static_cast<WavSink *>(user);
    if (sample_rate != KGV_SAMPLE_RATE ||
        sink->frames > (std::numeric_limits<std::uint32_t>::max() / 2U) - frame_count) {
        sink->failed = true;
        return 0;
    }
    std::vector<unsigned char> bytes(frame_count * 2U);
    for (std::size_t index = 0U; index < frame_count; ++index) {
        const std::uint16_t value = static_cast<std::uint16_t>(frames[index]);
        bytes[index * 2U] = static_cast<unsigned char>(value & 0xffU);
        bytes[index * 2U + 1U] = static_cast<unsigned char>(value >> 8U);
    }
    sink->stream.write(reinterpret_cast<const char *>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
    if (!sink->stream) {
        sink->failed = true;
        return 0;
    }
    sink->frames += static_cast<std::uint64_t>(frame_count);
    return 1;
}

kgv_engine *open_engine(const Arguments &arguments, char *error, std::size_t error_size,
                        int *status) {
    kgv_engine_options options{};
    options.struct_size = sizeof(options);
    options.abi_version = KILIX_VOICEGEN_ABI_VERSION;
    options.thread_count = 1U;
    options.flags = 0U;
    options.expected_release_sha256 = arguments.release_sha.c_str();
    kgv_engine *engine = nullptr;
    *status = kgv_engine_open(arguments.model.c_str(), &options, &engine, error, error_size);
    return engine;
}

int verify_command(const Arguments &arguments) {
    if (arguments.model.empty() || arguments.release_sha.empty()) {
        std::cerr << "verify requires --model and --release-sha\n";
        return 2;
    }
    std::array<char, 512> error{};
    int status = KGV_OK;
    kgv_engine *engine = open_engine(arguments, error.data(), error.size(), &status);
    if (status != KGV_OK) {
        std::cerr << kgv_status_name(status) << ": " << error.data() << '\n';
        return 1;
    }
    kgv_engine_close(engine);
    std::cout << "{\"abi_version\":1,\"schema\":\"kilix.voicegen.verification/v1\","
                 "\"status\":\"ok\"}\n";
    return 0;
}

int frontend_command(Arguments arguments) {
    if (arguments.profile.empty()) {
        std::cerr << "frontend requires --profile prose or terminal\n";
        return 2;
    }
    if (arguments.read_stdin == !arguments.text.empty()) {
        std::cerr << "frontend requires exactly one of --stdin or --text\n";
        return 2;
    }
    if (arguments.read_stdin) {
        std::string input_error;
        if (!read_bounded_stdin(&arguments.text, &input_error)) {
            std::cerr << input_error << '\n';
            return 2;
        }
    }
    std::uint32_t profile = 0U;
    if (arguments.profile == "prose") {
        profile = KGV_PROFILE_PROSE;
    } else if (arguments.profile == "terminal") {
        profile = KGV_PROFILE_TERMINAL;
    } else {
        std::cerr << "--profile must be prose or terminal\n";
        return 2;
    }
    if (!arguments.model.empty() || !arguments.release_sha.empty() ||
        !arguments.voice.empty() || !arguments.output.empty()) {
        std::cerr << "frontend does not accept model, release, voice, or output options\n";
        return 2;
    }

    kgv::LexicalFrontendResult result;
    kgv::FrontendFailure failure;
    const int status = kgv::run_lexical_frontend(arguments.text, profile, &result, &failure);
    std::cout << kgv::lexical_frontend_json(result, status, failure);
    return status == KGV_OK ? 0 : 1;
}

int synthesize_command(Arguments arguments) {
    if (arguments.model.empty() || arguments.release_sha.empty() || arguments.voice.empty() ||
        arguments.profile.empty() || arguments.output.empty()) {
        std::cerr << "synthesize requires model, release hash, voice, profile, and output\n";
        return 2;
    }
    if (arguments.read_stdin == !arguments.text.empty()) {
        std::cerr << "synthesize requires exactly one of --stdin or --text\n";
        return 2;
    }
    if (arguments.read_stdin) {
        std::string input_error;
        if (!read_bounded_stdin(&arguments.text, &input_error)) {
            std::cerr << input_error << '\n';
            return 2;
        }
    }
    std::uint32_t profile = 0U;
    if (arguments.profile == "prose") {
        profile = KGV_PROFILE_PROSE;
    } else if (arguments.profile == "terminal") {
        profile = KGV_PROFILE_TERMINAL;
    } else {
        std::cerr << "--profile must be prose or terminal\n";
        return 2;
    }
    std::error_code path_error;
    const bool output_exists = std::filesystem::exists(arguments.output, path_error);
    if (path_error) {
        std::cerr << "could not inspect output path\n";
        return 2;
    }
    if (output_exists) {
        std::cerr << "output path already exists; refusing to overwrite it\n";
        return 2;
    }

    std::array<char, 512> error{};
    int status = KGV_OK;
    kgv_engine *engine = open_engine(arguments, error.data(), error.size(), &status);
    if (status != KGV_OK) {
        std::cerr << kgv_status_name(status) << ": " << error.data() << '\n';
        return 1;
    }

    kgv_request request{};
    request.struct_size = sizeof(request);
    request.frontend.struct_size = sizeof(request.frontend);
    request.frontend.utf8_text = arguments.text.data();
    request.frontend.utf8_size = arguments.text.size();
    request.frontend.profile = profile;
    request.voice_id = arguments.voice.c_str();
    request.rate = arguments.rate;
    request.seed = arguments.seed;
    kgv_job *job = nullptr;
    status = kgv_job_create(engine, &request, &job, error.data(), error.size());
    if (status != KGV_OK) {
        std::cerr << kgv_status_name(status) << ": " << error.data() << '\n';
        kgv_engine_close(engine);
        return 1;
    }

    WavSink sink;
    sink.stream.open(arguments.output, std::ios::binary | std::ios::trunc);
    if (!sink.stream) {
        std::cerr << "could not create output WAV\n";
        kgv_job_destroy(job);
        kgv_engine_close(engine);
        return 1;
    }
    const auto empty_header = wav_header(0U);
    sink.stream.write(reinterpret_cast<const char *>(empty_header.data()),
                      static_cast<std::streamsize>(empty_header.size()));
    status = kgv_job_run(job, write_pcm, &sink, error.data(), error.size());
    kgv_job_destroy(job);
    kgv_engine_close(engine);

    if (status == KGV_OK && !sink.failed) {
        const std::uint32_t data_bytes = static_cast<std::uint32_t>(sink.frames * 2U);
        const auto final_header = wav_header(data_bytes);
        sink.stream.seekp(0, std::ios::beg);
        sink.stream.write(reinterpret_cast<const char *>(final_header.data()),
                          static_cast<std::streamsize>(final_header.size()));
        sink.stream.flush();
    }
    const bool output_ok = status == KGV_OK && !sink.failed && sink.stream.good();
    sink.stream.close();
    if (!output_ok) {
        std::error_code ignored;
        (void)std::filesystem::remove(arguments.output, ignored);
        if (sink.failed) {
            std::cerr << "PCM output failed or exceeded the WAV size limit\n";
        } else {
            std::cerr << kgv_status_name(status) << ": " << error.data() << '\n';
        }
        return 1;
    }
    std::cout << "wrote 24 kHz mono PCM WAV\n";
    return 0;
}

int run_cli(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << KGV_PROJECT_VERSION << '\n';
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--abi-version") {
        std::cout << kgv_abi_version() << '\n';
        return 0;
    }
    if (argc < 2) {
        usage(std::cerr);
        return 2;
    }
    const std::string_view command(argv[1]);
    Arguments arguments;
    std::string error;
    if (!parse_arguments(argc, argv, 2, &arguments, &error)) {
        std::cerr << error << '\n';
        usage(std::cerr);
        return 2;
    }
    if (command == "verify") {
        return verify_command(arguments);
    }
    if (command == "frontend") {
        return frontend_command(std::move(arguments));
    }
    if (command == "synthesize") {
        return synthesize_command(std::move(arguments));
    }
    std::cerr << "unknown command\n";
    usage(std::cerr);
    return 2;
}

}  // namespace

int main(int argc, char **argv) {
    try {
        return run_cli(argc, argv);
    } catch (const std::bad_alloc &) {
        std::cerr << "kilix-voicegen ran out of memory\n";
        return 1;
    } catch (const std::exception &) {
        std::cerr << "kilix-voicegen encountered an unexpected local runtime failure\n";
        return 1;
    }
}
