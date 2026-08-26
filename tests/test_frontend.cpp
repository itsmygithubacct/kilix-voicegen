#include "frontend/pipeline.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void validate_result(std::string_view input,
                     int status,
                     const kgv::LexicalFrontendResult &result,
                     const kgv::FrontendFailure &failure) {
    for (const kgv::FrontendDiagnostic &diagnostic : result.diagnostics) {
        require(diagnostic.span.byte_start <= diagnostic.span.byte_end,
                "diagnostic span is reversed");
        require(diagnostic.span.byte_end <= input.size(),
                "diagnostic span exceeds input");
    }
    if (status != KGV_OK) {
        require(result.words.empty(), "failed request retained partial words");
        require(result.phrases.empty(), "failed request retained partial phrases");
        require(!failure.code.empty(), "failed request has no failure code");
        return;
    }

    require(failure.code.empty(), "successful request retained failure code");
    for (const kgv::LexicalWord &word : result.words) {
        require(!word.normalized.empty(), "empty normalized word");
        require(word.span.byte_start < word.span.byte_end, "empty word span");
        require(word.span.byte_end <= input.size(), "word span exceeds input");
    }
    std::size_t expected_word = 0U;
    for (const kgv::LexicalPhrase &phrase : result.phrases) {
        require(phrase.word_start == expected_word, "phrase ranges are not contiguous");
        require(phrase.word_start <= phrase.word_end, "phrase range is reversed");
        require(phrase.word_end <= result.words.size(), "phrase range exceeds words");
        require(phrase.span.byte_start <= phrase.span.byte_end,
                "phrase span is reversed");
        require(phrase.span.byte_end <= input.size(), "phrase span exceeds input");
        expected_word = phrase.word_end;
    }
    require(expected_word == result.words.size(), "phrases do not cover all words");
}

void run_case(std::string_view input, std::uint32_t profile) {
    kgv::LexicalFrontendResult first;
    kgv::FrontendFailure first_failure;
    const int first_status =
        kgv::run_lexical_frontend(input, profile, &first, &first_failure);
    validate_result(input, first_status, first, first_failure);

    kgv::LexicalFrontendResult second;
    kgv::FrontendFailure second_failure;
    const int second_status =
        kgv::run_lexical_frontend(input, profile, &second, &second_failure);
    require(first_status == second_status, "repeat changed status");
    require(kgv::lexical_frontend_json(first, first_status, first_failure) ==
                kgv::lexical_frontend_json(second, second_status, second_failure),
            "repeat changed lexical trace");
}

}  // namespace

int main() {
    try {
        run_case("", KGV_PROFILE_PROSE);
        run_case("Don\xe2\x80\x99t stop.\n\nReady?", KGV_PROFILE_PROSE);
        run_case("\x1b[31merror\x1b[0m", KGV_PROFILE_TERMINAL);
        run_case("hello \xf0\x9f\x98\x80", KGV_PROFILE_TERMINAL);

        {
            const std::string input = "hello zzqvth";
            kgv::LexicalFrontendResult result;
            kgv::FrontendFailure failure;
            const int status = kgv::run_lexical_frontend(
                input, KGV_PROFILE_PROSE, &result, &failure);
            validate_result(input, status, result, failure);
            require(status == KGV_INVALID_TEXT, "unknown prose word was accepted");
            require(failure.code == "UNKNOWN_PRONUNCIATION",
                    "unknown prose word returned the wrong failure");
        }
        {
            const std::string input(4097U, 'a');
            kgv::LexicalFrontendResult result;
            kgv::FrontendFailure failure;
            const int status = kgv::run_lexical_frontend(
                input, KGV_PROFILE_PROSE, &result, &failure);
            validate_result(input, status, result, failure);
            require(status == KGV_INPUT_TOO_LARGE,
                    "oversized surface token was accepted");
            require(failure.code == "SURFACE_TOKEN_TOO_LARGE",
                    "oversized surface token returned the wrong failure");
        }
        {
            const std::string input = std::string(129U, '(') + "word" +
                                      std::string(129U, ')');
            kgv::LexicalFrontendResult result;
            kgv::FrontendFailure failure;
            const int status = kgv::run_lexical_frontend(
                input, KGV_PROFILE_PROSE, &result, &failure);
            validate_result(input, status, result, failure);
            require(status == KGV_INVALID_TEXT, "deep punctuation was accepted");
            require(failure.code == "NESTING_TOO_DEEP",
                    "deep punctuation returned the wrong failure");
        }

        std::mt19937_64 generator(0x4b47562d46524f4eULL);
        std::uniform_int_distribution<unsigned int> length_distribution(0U, 256U);
        std::uniform_int_distribution<unsigned int> byte_distribution(0U, 255U);
        for (std::size_t iteration = 0U; iteration < 10000U; ++iteration) {
            const std::size_t length = length_distribution(generator);
            std::string input(length, '\0');
            for (char &byte : input) {
                byte = static_cast<char>(byte_distribution(generator));
            }
            run_case(input, iteration % 2U == 0U
                                ? KGV_PROFILE_PROSE
                                : KGV_PROFILE_TERMINAL);
        }

        constexpr std::string_view alphabet =
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
            " .,;:!?/\\@_-=+%$#&*()[]{}\t\n\r";
        std::uniform_int_distribution<std::size_t> character_distribution(
            0U, alphabet.size() - 1U);
        for (std::size_t iteration = 0U; iteration < 10000U; ++iteration) {
            const std::size_t length = length_distribution(generator);
            std::string input;
            input.reserve(length);
            for (std::size_t index = 0U; index < length; ++index) {
                input.push_back(alphabet[character_distribution(generator)]);
            }
            run_case(input, iteration % 2U == 0U
                                ? KGV_PROFILE_PROSE
                                : KGV_PROFILE_TERMINAL);
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
