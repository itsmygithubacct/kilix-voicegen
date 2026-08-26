#include "frontend/pipeline.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kgv {
namespace {

constexpr std::size_t kMaximumSurfaceScalars = 4096U;
constexpr std::size_t kMaximumSurfaceBytes = 4096U;
constexpr std::size_t kMaximumNestingDepth = 128U;
constexpr std::size_t kLexicalChunkWordBudget = 256U;

bool ascii_letter(std::uint32_t scalar) noexcept {
    return (scalar >= static_cast<std::uint32_t>('A') &&
            scalar <= static_cast<std::uint32_t>('Z')) ||
           (scalar >= static_cast<std::uint32_t>('a') &&
            scalar <= static_cast<std::uint32_t>('z'));
}

bool whitespace(std::uint32_t scalar) noexcept {
    return scalar == 0x0009U || scalar == 0x000aU || scalar == 0x000dU ||
           scalar == 0x0020U || scalar == 0x00a0U || scalar == 0x1680U ||
           (scalar >= 0x2000U && scalar <= 0x200aU) || scalar == 0x2028U ||
           scalar == 0x2029U || scalar == 0x202fU || scalar == 0x205fU ||
           scalar == 0x3000U;
}

bool line_break(std::uint32_t scalar) noexcept {
    return scalar == 0x000aU || scalar == 0x000dU || scalar == 0x2028U ||
           scalar == 0x2029U;
}

bool latin_letter(std::uint32_t scalar) noexcept {
    return ascii_letter(scalar) || (scalar >= 0x00c0U && scalar <= 0x024fU) ||
           (scalar >= 0x1e00U && scalar <= 0x1effU);
}

char ascii_lower(char value) noexcept {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value + ('a' - 'A'));
    }
    return value;
}

std::string lower_ascii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), ascii_lower);
    return result;
}

bool all_ascii(std::string_view value) noexcept {
    return std::all_of(value.begin(), value.end(), [](char byte) {
        return static_cast<unsigned char>(byte) <= 0x7fU;
    });
}

bool all_digits(std::string_view value) noexcept {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](char byte) {
        return byte >= '0' && byte <= '9';
    });
}

SourceSpan scalar_span(const std::vector<FrontendScalar> &scalars,
                       std::size_t begin,
                       std::size_t end) noexcept {
    if (begin >= end || end > scalars.size()) {
        return SourceSpan{};
    }
    return SourceSpan{scalars[begin].byte_start, scalars[end - 1U].byte_end};
}

void append_utf8(std::uint32_t scalar, std::string *output) {
    if (scalar <= 0x7fU) {
        output->push_back(static_cast<char>(scalar));
    } else if (scalar <= 0x7ffU) {
        output->push_back(static_cast<char>(0xc0U | (scalar >> 6U)));
        output->push_back(static_cast<char>(0x80U | (scalar & 0x3fU)));
    } else if (scalar <= 0xffffU) {
        output->push_back(static_cast<char>(0xe0U | (scalar >> 12U)));
        output->push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3fU)));
        output->push_back(static_cast<char>(0x80U | (scalar & 0x3fU)));
    } else {
        output->push_back(static_cast<char>(0xf0U | (scalar >> 18U)));
        output->push_back(static_cast<char>(0x80U | ((scalar >> 12U) & 0x3fU)));
        output->push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3fU)));
        output->push_back(static_cast<char>(0x80U | (scalar & 0x3fU)));
    }
}

std::string scalar_text(std::string_view,
                        const std::vector<FrontendScalar> &scalars,
                        std::size_t begin,
                        std::size_t end) {
    std::string result;
    for (std::size_t index = begin; index < end; ++index) {
        append_utf8(scalars[index].value, &result);
    }
    return result;
}

const char *digit_word(char digit) noexcept {
    constexpr std::array<const char *, 10> names = {
        "zero", "one", "two", "three", "four",
        "five", "six", "seven", "eight", "nine",
    };
    return names[static_cast<std::size_t>(digit - '0')];
}

void append_under_thousand(std::uint64_t value, std::vector<std::string> *words) {
    constexpr std::array<const char *, 20> small = {
        "zero", "one", "two", "three", "four", "five", "six", "seven",
        "eight", "nine", "ten", "eleven", "twelve", "thirteen", "fourteen",
        "fifteen", "sixteen", "seventeen", "eighteen", "nineteen",
    };
    constexpr std::array<const char *, 10> tens = {
        "", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy",
        "eighty", "ninety",
    };
    if (value >= 100U) {
        words->emplace_back(small[static_cast<std::size_t>(value / 100U)]);
        words->emplace_back("hundred");
        value %= 100U;
    }
    if (value >= 20U) {
        words->emplace_back(tens[static_cast<std::size_t>(value / 10U)]);
        value %= 10U;
    }
    if (value > 0U) {
        words->emplace_back(small[static_cast<std::size_t>(value)]);
    }
}

std::vector<std::string> cardinal_words(std::uint64_t value) {
    constexpr std::array<const char *, 6> groups = {
        "", "thousand", "million", "billion", "trillion", "quadrillion",
    };
    if (value == 0U) {
        return {"zero"};
    }
    std::array<std::uint16_t, 6> parts{};
    std::size_t part_count = 0U;
    while (value > 0U && part_count < parts.size()) {
        parts[part_count] = static_cast<std::uint16_t>(value % 1000U);
        value /= 1000U;
        ++part_count;
    }
    std::vector<std::string> words;
    while (part_count > 0U) {
        --part_count;
        if (parts[part_count] == 0U) {
            continue;
        }
        append_under_thousand(parts[part_count], &words);
        if (part_count > 0U) {
            words.emplace_back(groups[part_count]);
        }
    }
    return words;
}

std::vector<std::string> ordinal_words(std::uint64_t value) {
    constexpr std::array<const char *, 20> small = {
        "zeroth", "first", "second", "third", "fourth", "fifth", "sixth",
        "seventh", "eighth", "ninth", "tenth", "eleventh", "twelfth",
        "thirteenth", "fourteenth", "fifteenth", "sixteenth", "seventeenth",
        "eighteenth", "nineteenth",
    };
    constexpr std::array<const char *, 10> tens = {
        "", "", "twentieth", "thirtieth", "fortieth", "fiftieth",
        "sixtieth", "seventieth", "eightieth", "ninetieth",
    };
    if (value < small.size()) {
        return {small[static_cast<std::size_t>(value)]};
    }
    if (value < 100U && value % 10U == 0U) {
        return {tens[static_cast<std::size_t>(value / 10U)]};
    }
    if (value < 100U) {
        std::vector<std::string> words = cardinal_words(value - (value % 10U));
        const std::vector<std::string> tail = ordinal_words(value % 10U);
        words.insert(words.end(), tail.begin(), tail.end());
        return words;
    }
    std::vector<std::string> words = cardinal_words(value);
    if (!words.empty()) {
        std::string &last = words.back();
        if (last == "one") last = "first";
        else if (last == "two") last = "second";
        else if (last == "three") last = "third";
        else if (last == "five") last = "fifth";
        else if (last == "eight") last = "eighth";
        else if (last == "nine") last = "ninth";
        else if (last == "twelve") last = "twelfth";
        else if (last == "twenty") last = "twentieth";
        else if (last == "thirty") last = "thirtieth";
        else if (last == "forty") last = "fortieth";
        else if (last == "fifty") last = "fiftieth";
        else if (last == "sixty") last = "sixtieth";
        else if (last == "seventy") last = "seventieth";
        else if (last == "eighty") last = "eightieth";
        else if (last == "ninety") last = "ninetieth";
        else last.append("th");
    }
    return words;
}

bool parse_u64(std::string_view text, std::uint64_t *value) noexcept {
    if (text.empty()) {
        return false;
    }
    const auto converted =
        std::from_chars(text.data(), text.data() + text.size(), *value, 10);
    return converted.ec == std::errc{} && converted.ptr == text.data() + text.size();
}

std::vector<std::string> year_words(std::uint64_t year) {
    if (year >= 2000U && year <= 2099U && year != 2000U) {
        std::vector<std::string> words = {"twenty"};
        const std::uint64_t remainder = year - 2000U;
        if (remainder < 10U) {
            words.emplace_back("oh");
            words.emplace_back(digit_word(static_cast<char>('0' + remainder)));
        } else {
            const std::vector<std::string> tail = cardinal_words(remainder);
            words.insert(words.end(), tail.begin(), tail.end());
        }
        return words;
    }
    return cardinal_words(year);
}

bool leap_year(std::uint64_t year) noexcept {
    return (year % 4U == 0U && year % 100U != 0U) || year % 400U == 0U;
}

bool valid_date(std::uint64_t year, std::uint64_t month,
                std::uint64_t day) noexcept {
    constexpr std::array<std::uint64_t, 12> month_days = {
        31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U,
    };
    if (year == 0U || month == 0U || month > 12U || day == 0U) {
        return false;
    }
    std::uint64_t maximum = month_days[static_cast<std::size_t>(month - 1U)];
    if (month == 2U && leap_year(year)) {
        maximum = 29U;
    }
    return day <= maximum;
}

std::string hex_scalar(std::uint32_t value) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    do {
        result.push_back(digits[value & 0x0fU]);
        value >>= 4U;
    } while (value != 0U);
    std::reverse(result.begin(), result.end());
    return result;
}

bool ascii_word_like(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    for (std::size_t index = 0U; index < value.size(); ++index) {
        const char byte = value[index];
        const bool letter = (byte >= 'A' && byte <= 'Z') ||
                            (byte >= 'a' && byte <= 'z');
        const bool apostrophe = (byte == '\'' && index > 0U &&
                                 index + 1U < value.size());
        if (!letter && !apostrophe) {
            return false;
        }
    }
    return true;
}

bool uppercase_word(std::string_view value) noexcept {
    bool saw_letter = false;
    for (char byte : value) {
        if (byte >= 'a' && byte <= 'z') {
            return false;
        }
        if (byte >= 'A' && byte <= 'Z') {
            saw_letter = true;
        }
    }
    return saw_letter;
}

bool contains_vowel(std::string_view value) noexcept {
    for (char byte : value) {
        switch (ascii_lower(byte)) {
            case 'a': case 'e': case 'i': case 'o': case 'u': case 'y':
                return true;
            default:
                break;
        }
    }
    return false;
}

class Builder final {
public:
    Builder(std::string_view source,
            std::uint32_t requested_profile,
            LexicalFrontendResult *output,
            FrontendFailure *output_failure)
        : text(source), profile(requested_profile), result(output), failure(output_failure) {}

    void add_word(std::string word, SourceSpan span,
                  std::string source_kind = "LITERAL") {
        result->words.push_back(LexicalWord{
            std::move(word), std::move(source_kind), span,
        });
        last_word_end = span.byte_end;
        if (!phrase_has_span) {
            phrase_byte_start = span.byte_start;
            phrase_has_span = true;
        }
    }

    void add_words(const std::vector<std::string> &words, SourceSpan span,
                   std::string_view source_kind = "EXPANSION") {
        for (const std::string &word : words) {
            add_word(word, span, std::string(source_kind));
        }
    }

    void add_diagnostic(std::string code, std::string severity, SourceSpan span) {
        result->diagnostics.push_back(FrontendDiagnostic{
            std::move(code), std::move(severity), span,
        });
    }

    void end_phrase(PhraseTerminator terminator, SourceSpan boundary_span,
                    bool retain_empty = false) {
        const std::size_t word_end = result->words.size();
        if (word_end == phrase_word_start && !retain_empty) {
            return;
        }
        SourceSpan span = boundary_span;
        if (phrase_has_span) {
            span.byte_start = phrase_byte_start;
            span.byte_end = std::max(last_word_end, boundary_span.byte_end);
        }
        result->phrases.push_back(LexicalPhrase{
            phrase_word_start, word_end, terminator, span,
        });
        phrase_word_start = word_end;
        phrase_has_span = false;
        last_quantity_is_one = false;
        last_token_is_quantity = false;
    }

    int reject(std::string code, std::string message, SourceSpan span) {
        add_diagnostic(code, "ERROR", span);
        failure->status = KGV_INVALID_TEXT;
        failure->code = std::move(code);
        failure->message = std::move(message);
        failure->byte_offset = span.byte_start;
        return KGV_INVALID_TEXT;
    }

    void finish() {
        if (result->words.size() > phrase_word_start) {
            const SourceSpan tail{phrase_byte_start, last_word_end};
            end_phrase(PhraseTerminator::none, tail);
        }
        apply_lexical_chunk_budget();
    }

    std::string_view text;
    std::uint32_t profile;
    LexicalFrontendResult *result;
    FrontendFailure *failure;
    bool last_quantity_is_one = false;
    bool last_token_is_quantity = false;

private:
    void apply_lexical_chunk_budget() {
        std::vector<LexicalPhrase> bounded;
        for (const LexicalPhrase &phrase : result->phrases) {
            std::size_t start = phrase.word_start;
            while (phrase.word_end - start > kLexicalChunkWordBudget) {
                const std::size_t end = start + kLexicalChunkWordBudget;
                bounded.push_back(LexicalPhrase{
                    start,
                    end,
                    PhraseTerminator::continuation,
                    SourceSpan{result->words[start].span.byte_start,
                               result->words[end - 1U].span.byte_end},
                });
                start = end;
            }
            if (start < phrase.word_end || phrase.word_start == phrase.word_end) {
                LexicalPhrase tail = phrase;
                tail.word_start = start;
                if (start < phrase.word_end) {
                    tail.span.byte_start = result->words[start].span.byte_start;
                }
                bounded.push_back(std::move(tail));
            }
        }
        result->phrases = std::move(bounded);
    }

    std::size_t phrase_word_start = 0U;
    std::size_t phrase_byte_start = 0U;
    std::size_t last_word_end = 0U;
    bool phrase_has_span = false;
};

void spell_digits(std::string_view digits, Builder *builder, SourceSpan span) {
    for (char digit : digits) {
        builder->add_word(digit_word(digit), span, "SPELLING");
    }
}

bool valid_grouping(std::string_view value) noexcept {
    const std::size_t first_comma = value.find(',');
    if (first_comma == std::string_view::npos || first_comma == 0U || first_comma > 3U) {
        return false;
    }
    if (!all_digits(value.substr(0U, first_comma))) {
        return false;
    }
    std::size_t start = first_comma + 1U;
    while (start < value.size()) {
        const std::size_t comma = value.find(',', start);
        const std::size_t end = comma == std::string_view::npos ? value.size() : comma;
        if (end - start != 3U || !all_digits(value.substr(start, 3U))) {
            return false;
        }
        if (comma == std::string_view::npos) {
            return true;
        }
        start = comma + 1U;
    }
    return false;
}

bool expand_integer(std::string_view value, Builder *builder, SourceSpan span) {
    if (!all_digits(value)) {
        return false;
    }
    if (value.size() > 1U && value.front() == '0') {
        spell_digits(value, builder, span);
        builder->last_quantity_is_one = false;
        builder->last_token_is_quantity = true;
        return true;
    }
    if (value.size() > 18U) {
        builder->add_diagnostic("CARDINAL_TOO_LARGE", "WARNING", span);
        spell_digits(value, builder, span);
        builder->last_quantity_is_one = false;
        builder->last_token_is_quantity = true;
        return true;
    }
    std::uint64_t number = 0U;
    if (!parse_u64(value, &number)) {
        return false;
    }
    builder->add_words(cardinal_words(number), span);
    builder->last_quantity_is_one = number == 1U;
    builder->last_token_is_quantity = true;
    return true;
}

bool expand_number(std::string_view value, Builder *builder, SourceSpan span) {
    char sign = '\0';
    if (!value.empty() && (value.front() == '-' || value.front() == '+')) {
        sign = value.front();
        value.remove_prefix(1U);
    }
    if (value.empty()) {
        return false;
    }
    const auto add_sign = [&]() {
        if (sign == '-') {
            builder->add_word("minus", span, "EXPANSION");
        } else if (sign == '+') {
            builder->add_word("plus", span, "EXPANSION");
        }
    };
    const std::size_t decimal = value.find('.');
    if (decimal != std::string_view::npos) {
        if (value.find('.', decimal + 1U) != std::string_view::npos ||
            !all_digits(value.substr(0U, decimal)) ||
            !all_digits(value.substr(decimal + 1U))) {
            return false;
        }
        add_sign();
        (void)expand_integer(value.substr(0U, decimal), builder, span);
        builder->add_word("point", span, "EXPANSION");
        spell_digits(value.substr(decimal + 1U), builder, span);
        builder->last_quantity_is_one = false;
        builder->last_token_is_quantity = true;
        return true;
    }
    if (value.find(',') != std::string_view::npos) {
        if (!std::all_of(value.begin(), value.end(), [](char byte) {
                return (byte >= '0' && byte <= '9') || byte == ',';
            })) {
            return false;
        }
        add_sign();
        if (!valid_grouping(value)) {
            builder->add_diagnostic("INVALID_DIGIT_GROUPING", "WARNING", span);
            std::size_t start = 0U;
            while (start < value.size()) {
                const std::size_t comma = value.find(',', start);
                const std::size_t end = comma == std::string_view::npos ? value.size() : comma;
                (void)expand_integer(value.substr(start, end - start), builder, span);
                if (comma == std::string_view::npos) {
                    break;
                }
                builder->add_word("comma", span, "EXPANSION");
                start = comma + 1U;
            }
            return true;
        }
        std::string compact;
        compact.reserve(value.size());
        std::copy_if(value.begin(), value.end(), std::back_inserter(compact),
                     [](char byte) { return byte != ','; });
        value = compact;
        return expand_integer(value, builder, span);
    }
    if (!all_digits(value)) {
        return false;
    }
    add_sign();
    return expand_integer(value, builder, span);
}

bool expand_ordinal(std::string_view value, Builder *builder, SourceSpan span) {
    if (value.size() < 3U) {
        return false;
    }
    const std::string suffix = lower_ascii(value.substr(value.size() - 2U));
    if (suffix != "st" && suffix != "nd" && suffix != "rd" && suffix != "th") {
        return false;
    }
    const std::string_view digits = value.substr(0U, value.size() - 2U);
    std::uint64_t number = 0U;
    if (!all_digits(digits) || digits.size() > 18U || !parse_u64(digits, &number)) {
        return false;
    }
    const std::uint64_t modulo100 = number % 100U;
    const std::uint64_t modulo10 = number % 10U;
    std::string expected = "th";
    if (modulo100 < 11U || modulo100 > 13U) {
        if (modulo10 == 1U) expected = "st";
        if (modulo10 == 2U) expected = "nd";
        if (modulo10 == 3U) expected = "rd";
    }
    if (suffix != expected) {
        return false;
    }
    builder->add_words(ordinal_words(number), span);
    builder->last_quantity_is_one = false;
    builder->last_token_is_quantity = false;
    return true;
}

bool expand_iso_date(std::string_view value, Builder *builder, SourceSpan span) {
    if (value.size() != 10U || value[4] != '-' || value[7] != '-' ||
        !all_digits(value.substr(0U, 4U)) || !all_digits(value.substr(5U, 2U)) ||
        !all_digits(value.substr(8U, 2U))) {
        return false;
    }
    std::uint64_t year = 0U;
    std::uint64_t month = 0U;
    std::uint64_t day = 0U;
    (void)parse_u64(value.substr(0U, 4U), &year);
    (void)parse_u64(value.substr(5U, 2U), &month);
    (void)parse_u64(value.substr(8U, 2U), &day);
    if (!valid_date(year, month, day)) {
        return false;
    }
    constexpr std::array<const char *, 12> months = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December",
    };
    builder->add_word("the", span, "EXPANSION");
    builder->add_words(ordinal_words(day), span);
    builder->add_word("of", span, "EXPANSION");
    builder->add_word(months[static_cast<std::size_t>(month - 1U)], span, "EXPANSION");
    builder->add_words(year_words(year), span);
    builder->last_quantity_is_one = false;
    builder->last_token_is_quantity = false;
    return true;
}

bool expand_ambiguous_date(std::string_view value, Builder *builder, SourceSpan span) {
    const std::size_t first = value.find('/');
    const std::size_t second = first == std::string_view::npos
        ? std::string_view::npos : value.find('/', first + 1U);
    if (first == std::string_view::npos || second == std::string_view::npos ||
        value.find('/', second + 1U) != std::string_view::npos ||
        !all_digits(value.substr(0U, first)) ||
        !all_digits(value.substr(first + 1U, second - first - 1U)) ||
        !all_digits(value.substr(second + 1U))) {
        return false;
    }
    builder->add_diagnostic("AMBIGUOUS_DATE", "WARNING", span);
    (void)expand_integer(value.substr(0U, first), builder, span);
    builder->add_word("slash", span, "EXPANSION");
    (void)expand_integer(value.substr(first + 1U, second - first - 1U), builder, span);
    builder->add_word("slash", span, "EXPANSION");
    (void)expand_integer(value.substr(second + 1U), builder, span);
    builder->last_quantity_is_one = false;
    builder->last_token_is_quantity = false;
    return true;
}

bool expand_time(std::string_view value, Builder *builder, SourceSpan span) {
    const std::size_t colon = value.find(':');
    if (colon == std::string_view::npos || value.find(':', colon + 1U) != std::string_view::npos ||
        colon == 0U || colon > 2U || value.size() - colon - 1U != 2U ||
        !all_digits(value.substr(0U, colon)) || !all_digits(value.substr(colon + 1U))) {
        return false;
    }
    std::uint64_t hour = 0U;
    std::uint64_t minute = 0U;
    (void)parse_u64(value.substr(0U, colon), &hour);
    (void)parse_u64(value.substr(colon + 1U), &minute);
    if (hour > 23U || minute > 59U) {
        return false;
    }
    builder->add_words(cardinal_words(hour), span);
    if (minute < 10U) {
        builder->add_word("oh", span, "EXPANSION");
        builder->add_word(digit_word(static_cast<char>('0' + minute)), span, "EXPANSION");
    } else {
        builder->add_words(cardinal_words(minute), span);
    }
    builder->last_quantity_is_one = false;
    builder->last_token_is_quantity = false;
    return true;
}

bool expand_currency(std::string_view value, Builder *builder, SourceSpan span) {
    std::string_view symbol;
    if (!value.empty() && value.front() == '$') {
        symbol = "$";
    } else if (value.size() >= 2U && value.substr(0U, 2U) == "\xc2\xa3") {
        symbol = "\xc2\xa3";
    } else if (value.size() >= 3U && value.substr(0U, 3U) == "\xe2\x82\xac") {
        symbol = "\xe2\x82\xac";
    } else {
        return false;
    }
    value.remove_prefix(symbol.size());
    const std::size_t point = value.find('.');
    const std::string_view major = point == std::string_view::npos
        ? value : value.substr(0U, point);
    const std::string_view minor = point == std::string_view::npos
        ? std::string_view{} : value.substr(point + 1U);
    if (!all_digits(major) || major.size() > 18U ||
        (point != std::string_view::npos &&
         (minor.empty() || !all_digits(minor) || minor.size() > 2U))) {
        return false;
    }
    std::uint64_t major_value = 0U;
    (void)parse_u64(major, &major_value);
    builder->add_words(cardinal_words(major_value), span);
    if (symbol == "$") {
        builder->add_word(major_value == 1U ? "dollar" : "dollars", span, "EXPANSION");
    } else if (symbol == "\xc2\xa3") {
        builder->add_word(major_value == 1U ? "pound" : "pounds", span, "EXPANSION");
    } else {
        builder->add_word(major_value == 1U ? "euro" : "euros", span, "EXPANSION");
    }
    if (!minor.empty()) {
        std::string padded(minor);
        if (padded.size() == 1U) {
            padded.push_back('0');
        }
        std::uint64_t minor_value = 0U;
        (void)parse_u64(padded, &minor_value);
        if (minor_value > 0U) {
            builder->add_word("and", span, "EXPANSION");
            builder->add_words(cardinal_words(minor_value), span);
            if (symbol == "\xc2\xa3") {
                builder->add_word(minor_value == 1U ? "penny" : "pence",
                                  span, "EXPANSION");
            } else {
                builder->add_word(minor_value == 1U ? "cent" : "cents",
                                  span, "EXPANSION");
            }
        }
    }
    builder->last_quantity_is_one = false;
    builder->last_token_is_quantity = false;
    return true;
}

bool expand_unit(std::string_view value, Builder *builder, SourceSpan span) {
    struct Unit final {
        std::string_view symbol;
        std::string_view singular;
        std::string_view plural;
    };
    constexpr std::array<Unit, 15> units = {{
        {"Hz", "hertz", "hertz"}, {"kHz", "kilohertz", "kilohertz"},
        {"MHz", "megahertz", "megahertz"}, {"GHz", "gigahertz", "gigahertz"},
        {"B", "byte", "bytes"}, {"KiB", "kibibyte", "kibibytes"},
        {"MiB", "mebibyte", "mebibytes"}, {"GiB", "gibibyte", "gibibytes"},
        {"km", "kilometre", "kilometres"}, {"m", "metre", "metres"},
        {"cm", "centimetre", "centimetres"}, {"mm", "millimetre", "millimetres"},
        {"kg", "kilogram", "kilograms"}, {"g", "gram", "grams"},
        {"%", "percent", "percent"},
    }};
    for (const Unit &unit : units) {
        if (value == unit.symbol && builder->last_token_is_quantity) {
            builder->add_word(std::string(builder->last_quantity_is_one
                                              ? unit.singular : unit.plural),
                              span, "EXPANSION");
            builder->last_quantity_is_one = false;
            builder->last_token_is_quantity = false;
            return true;
        }
    }
    return false;
}

void spell_letters(std::string_view value, Builder *builder, SourceSpan span) {
    for (char byte : value) {
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z')) {
            builder->add_word(std::string(1U, static_cast<char>(
                byte >= 'a' ? byte - ('a' - 'A') : byte)), span, "SPELLING");
        }
    }
}

std::string_view terminal_symbol_name(char value) noexcept {
    switch (value) {
        case '-': return "dash";
        case '/': return "slash";
        case '.': return "dot";
        case ':': return "colon";
        case '@': return "at";
        case '_': return "underscore";
        case '?': return "question";
        case '=': return "equals";
        case '&': return "and";
        case '$': return "dollar";
        case '>': return "greater than";
        case '<': return "less than";
        case '~': return "tilde";
        case '\\': return "backslash";
        case '#': return "hash";
        case '%': return "percent";
        case '+': return "plus";
        case '*': return "star";
        case '|': return "pipe";
        case '!': return "exclamation";
        case '[': return "left bracket";
        case ']': return "right bracket";
        case '{': return "left brace";
        case '}': return "right brace";
        case '(': return "left parenthesis";
        case ')': return "right parenthesis";
        case ',': return "comma";
        case ';': return "semicolon";
        case '^': return "caret";
        case '`': return "backtick";
        case '\"': return "double quote";
        case '\'': return "single quote";
        default: return {};
    }
}

void add_terminal_symbol(char value, Builder *builder, SourceSpan span) {
    const std::string_view name = terminal_symbol_name(value);
    std::size_t start = 0U;
    while (start < name.size()) {
        const std::size_t space = name.find(' ', start);
        const std::size_t end = space == std::string_view::npos ? name.size() : space;
        builder->add_word(std::string(name.substr(start, end - start)), span, "SPELLING");
        if (space == std::string_view::npos) {
            break;
        }
        start = space + 1U;
    }
}

bool terminal_structural(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    constexpr std::string_view structural = "/:@._-=<>|&$~*[]{}\\?#%+!,;()^`\"";
    return value.find_first_of(structural) != std::string_view::npos;
}

bool terminal_identifier(std::string_view value) noexcept {
    bool saw_letter = false;
    bool saw_digit = false;
    bool saw_case_boundary = false;
    for (std::size_t index = 0U; index < value.size(); ++index) {
        const char byte = value[index];
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z')) {
            saw_letter = true;
        } else if (byte >= '0' && byte <= '9') {
            saw_digit = true;
        } else {
            return false;
        }
        if (index > 0U && byte >= 'A' && byte <= 'Z' &&
            value[index - 1U] >= 'a' && value[index - 1U] <= 'z') {
            saw_case_boundary = true;
        }
    }
    return saw_letter && (saw_digit || saw_case_boundary);
}

bool ascii_letters(std::string_view value) noexcept {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](char byte) {
               return (byte >= 'A' && byte <= 'Z') ||
                      (byte >= 'a' && byte <= 'z');
           });
}

bool hexadecimal_payload(std::string_view value) noexcept {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](char byte) {
               return (byte >= '0' && byte <= '9') ||
                      (byte >= 'A' && byte <= 'F') ||
                      (byte >= 'a' && byte <= 'f');
           });
}

void emit_terminal_run(std::string_view run, bool force_letters,
                       Builder *builder, SourceSpan span) {
    if (force_letters || uppercase_word(run)) {
        spell_letters(run, builder, span);
        return;
    }
    std::size_t part = 0U;
    for (std::size_t cursor = 1U; cursor < run.size(); ++cursor) {
        const bool lower_to_upper =
            run[cursor - 1U] >= 'a' && run[cursor - 1U] <= 'z' &&
            run[cursor] >= 'A' && run[cursor] <= 'Z';
        const bool acronym_to_word =
            cursor + 1U < run.size() &&
            run[cursor - 1U] >= 'A' && run[cursor - 1U] <= 'Z' &&
            run[cursor] >= 'A' && run[cursor] <= 'Z' &&
            run[cursor + 1U] >= 'a' && run[cursor + 1U] <= 'z';
        if (!lower_to_upper && !acronym_to_word) {
            continue;
        }
        const std::string_view piece = run.substr(part, cursor - part);
        if (uppercase_word(piece)) {
            spell_letters(piece, builder, span);
        } else {
            builder->add_word(lower_ascii(piece), span, "SPELLING");
        }
        part = cursor;
    }
    const std::string_view piece = run.substr(part);
    if (uppercase_word(piece) && piece.size() > 1U) {
        spell_letters(piece, builder, span);
    } else {
        builder->add_word(lower_ascii(piece), span, "SPELLING");
    }
}

void expand_terminal_atom(std::string_view value, Builder *builder, SourceSpan span) {
    if (value == "README.md") {
        builder->add_word("read", span, "EXPANSION");
        builder->add_word("me", span, "EXPANSION");
        builder->add_word("dot", span, "SPELLING");
        builder->add_word("M", span, "SPELLING");
        builder->add_word("D", span, "SPELLING");
        return;
    }
    if (value.size() > 7U && lower_ascii(value.substr(0U, 7U)) == "sha256:" &&
        hexadecimal_payload(value.substr(7U))) {
        spell_letters("SHA", builder, span);
        spell_digits("256", builder, span);
        add_terminal_symbol(':', builder, span);
        for (char byte : value.substr(7U)) {
            if (byte >= '0' && byte <= '9') {
                builder->add_word(digit_word(byte), span, "SPELLING");
            } else {
                builder->add_word(std::string(1U, static_cast<char>(
                    byte >= 'a' ? byte - ('a' - 'A') : byte)), span, "SPELLING");
            }
        }
        builder->last_quantity_is_one = false;
        builder->last_token_is_quantity = false;
        return;
    }
    const bool uri = value.find("://") != std::string_view::npos;
    const bool email = value.find('@') != std::string_view::npos;
    const bool long_flag = value.size() > 2U && value.substr(0U, 2U) == "--";
    const bool short_flag = value.size() > 1U && value.front() == '-' &&
                            value[1U] != '-' && ascii_letters(value.substr(1U));
    std::size_t port_separator = std::string_view::npos;
    if (!uri) {
        const std::size_t colon = value.rfind(':');
        if (colon != std::string_view::npos && colon > 0U &&
            all_digits(value.substr(colon + 1U))) {
            port_separator = colon;
        }
    }
    std::size_t index = 0U;
    if (value.size() > 1U && value.front() == 'v' &&
        value[1] >= '0' && value[1] <= '9') {
        builder->add_word("version", span, "EXPANSION");
        ++index;
    }
    while (index < value.size()) {
        const char byte = value[index];
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z')) {
            std::size_t end = index + 1U;
            while (end < value.size() &&
                   ((value[end] >= 'A' && value[end] <= 'Z') ||
                    (value[end] >= 'a' && value[end] <= 'z'))) {
                ++end;
            }
            const std::string_view run = value.substr(index, end - index);
            const bool scheme = end + 2U < value.size() && value.substr(end, 3U) == "://";
            const bool extension = !uri && !email && index > 0U &&
                                   value[index - 1U] == '.' && run.size() <= 4U;
            if (scheme || extension || uppercase_word(run)) {
                spell_letters(run, builder, span);
            } else {
                emit_terminal_run(run, short_flag && index > 0U, builder, span);
            }
            index = end;
            continue;
        }
        if (byte >= '0' && byte <= '9') {
            builder->add_word(digit_word(byte), span, "SPELLING");
            ++index;
            continue;
        }
        if (long_flag && byte == '-' && index >= 2U) {
            ++index;
            continue;
        }
        if (index == port_separator) {
            builder->add_word("port", span, "EXPANSION");
        } else {
            add_terminal_symbol(byte, builder, span);
        }
        ++index;
    }
    builder->last_quantity_is_one = false;
    builder->last_token_is_quantity = false;
}

int expand_word(std::string_view value, SourceSpan span, Builder *builder);

int expand_unicode_terminal(const std::vector<FrontendScalar> &scalars,
                            std::size_t begin,
                            std::size_t end,
                            Builder *builder) {
    std::size_t index = begin;
    while (index < end) {
        const FrontendScalar &scalar = scalars[index];
        if (latin_letter(scalar.value)) {
            const std::size_t run_begin = index;
            while (index < end && (latin_letter(scalars[index].value) ||
                                   scalars[index].value == 0x0027U ||
                                   scalars[index].value == 0x2019U)) {
                ++index;
            }
            const SourceSpan run_span = scalar_span(scalars, run_begin, index);
            const std::string run = scalar_text(builder->text, scalars, run_begin, index);
            const int status = expand_word(run, run_span, builder);
            if (status != KGV_OK) {
                return status;
            }
            continue;
        }
        const SourceSpan span{scalar.byte_start, scalar.byte_end};
        if (scalar.value >= static_cast<std::uint32_t>('0') &&
            scalar.value <= static_cast<std::uint32_t>('9')) {
            builder->add_word(digit_word(static_cast<char>(scalar.value)), span,
                              "SPELLING");
            ++index;
            continue;
        }
        if (scalar.value <= 0x7fU) {
            const std::string_view symbol = terminal_symbol_name(
                static_cast<char>(scalar.value));
            if (!symbol.empty()) {
                add_terminal_symbol(static_cast<char>(scalar.value), builder, span);
                ++index;
                continue;
            }
        }
        builder->add_word("U", span, "SPELLING");
        builder->add_word("plus", span, "SPELLING");
        const std::string hexadecimal = hex_scalar(scalar.value);
        for (char digit : hexadecimal) {
            if (digit >= '0' && digit <= '9') {
                builder->add_word(digit_word(digit), span, "SPELLING");
            } else {
                builder->add_word(std::string(1U, digit), span, "SPELLING");
            }
        }
        builder->add_diagnostic("UNSUPPORTED_SCALAR", "WARNING", span);
        ++index;
    }
    return KGV_OK;
}

bool curly_contraction(std::string_view value) noexcept {
    return value.find("\xe2\x80\x99") != std::string_view::npos;
}

std::string canonical_word(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0U; index < value.size();) {
        if (index + 3U <= value.size() && value.substr(index, 3U) == "\xe2\x80\x99") {
            result.push_back('\'');
            index += 3U;
        } else {
            result.push_back(value[index]);
            ++index;
        }
    }
    return result;
}

int expand_word(std::string_view value, SourceSpan span, Builder *builder) {
    builder->last_quantity_is_one = false;
    builder->last_token_is_quantity = false;
    const std::string canonical = canonical_word(value);
    if (canonical == "Dr" || canonical == "Dr.") {
        builder->add_word("doctor", span, "ALIAS");
        return KGV_OK;
    }
    if (all_ascii(canonical) && ascii_word_like(canonical)) {
        const std::string folded = lower_ascii(canonical);
        if (folded.size() >= 5U && !contains_vowel(folded)) {
            if (builder->profile == KGV_PROFILE_TERMINAL) {
                spell_letters(canonical, builder, span);
                builder->add_diagnostic("FALLBACK_SPELLING", "WARNING", span);
                return KGV_OK;
            }
            return builder->reject("UNKNOWN_PRONUNCIATION",
                                   "word has no admitted lexical or LTS resolution", span);
        }
        if (uppercase_word(canonical) && canonical.size() > 1U) {
            if (canonical == "NASA") {
                builder->add_word("NASA", span, "LITERAL");
            } else {
                spell_letters(canonical, builder, span);
            }
            return KGV_OK;
        }
        builder->add_word(canonical, span, "LITERAL");
        return KGV_OK;
    }
    if (curly_contraction(value)) {
        builder->add_word(canonical, span, "LITERAL");
        return KGV_OK;
    }
    builder->add_word(canonical, span, "LITERAL");
    return KGV_OK;
}

bool range_value(std::string_view value,
                 std::string_view *left,
                 std::string_view *right) noexcept {
    const std::size_t dash = value.find('-');
    if (dash == std::string_view::npos || dash == 0U || dash + 1U >= value.size() ||
        value.find('-', dash + 1U) != std::string_view::npos) {
        return false;
    }
    *left = value.substr(0U, dash);
    *right = value.substr(dash + 1U);
    return all_digits(*left) && all_digits(*right) &&
           (left->size() == 1U || left->front() != '0') &&
           (right->size() == 1U || right->front() != '0');
}

int process_core(std::string_view value,
                 const std::vector<FrontendScalar> &scalars,
                 std::size_t scalar_begin,
                 std::size_t scalar_end,
                 Builder *builder) {
    if (value.empty()) {
        return KGV_OK;
    }
    const SourceSpan span = scalar_span(scalars, scalar_begin, scalar_end);
    if (expand_currency(value, builder, span)) {
        return KGV_OK;
    }
    if (!all_ascii(value)) {
        bool unsupported = false;
        for (std::size_t index = scalar_begin; index < scalar_end; ++index) {
            const std::uint32_t scalar = scalars[index].value;
            if (!latin_letter(scalar) && scalar != 0x2019U && scalar != 0x0027U) {
                unsupported = true;
                break;
            }
        }
        if (unsupported) {
            if (builder->profile == KGV_PROFILE_TERMINAL) {
                return expand_unicode_terminal(scalars, scalar_begin, scalar_end, builder);
            }
            return builder->reject("UNSUPPORTED_SCALAR",
                                   "scalar is outside the v1 prose inventory", span);
        }
        return expand_word(value, span, builder);
    }

    if (expand_iso_date(value, builder, span) ||
        expand_ambiguous_date(value, builder, span) ||
        expand_time(value, builder, span) ||
        expand_ordinal(value, builder, span)) {
        return KGV_OK;
    }
    std::string_view left;
    std::string_view right;
    if (range_value(value, &left, &right)) {
        (void)expand_integer(left, builder, span);
        builder->add_word(builder->profile == KGV_PROFILE_PROSE ? "to" : "dash",
                          span, "EXPANSION");
        (void)expand_integer(right, builder, span);
        return KGV_OK;
    }
    if (expand_unit(value, builder, span)) {
        return KGV_OK;
    }
    if (builder->profile == KGV_PROFILE_TERMINAL &&
        (terminal_structural(value) || terminal_identifier(value))) {
        expand_terminal_atom(value, builder, span);
        return KGV_OK;
    }
    if (expand_number(value, builder, span)) {
        return KGV_OK;
    }
    builder->last_quantity_is_one = false;
    builder->last_token_is_quantity = false;
    return expand_word(value, span, builder);
}

PhraseTerminator punctuation_terminator(char value) noexcept {
    switch (value) {
        case ',': return PhraseTerminator::comma;
        case ':': return PhraseTerminator::colon;
        case ';': return PhraseTerminator::semicolon;
        case '.': return PhraseTerminator::period;
        case '?': return PhraseTerminator::question;
        case '!': return PhraseTerminator::exclamation;
        default: return PhraseTerminator::none;
    }
}

bool dotted_initialism(std::string_view value) noexcept {
    if (value.size() < 3U || value.size() % 2U == 0U) {
        return false;
    }
    std::size_t letters = 0U;
    for (std::size_t index = 0U; index < value.size(); ++index) {
        if (index % 2U == 0U) {
            const char byte = value[index];
            if (!((byte >= 'A' && byte <= 'Z') ||
                  (byte >= 'a' && byte <= 'z'))) {
                return false;
            }
            ++letters;
        } else if (value[index] != '.') {
            return false;
        }
    }
    return letters >= 2U;
}

bool prose_protected_atom(std::string_view value) noexcept {
    if (value.find("://") != std::string_view::npos ||
        value.find('@') != std::string_view::npos ||
        value.find('/') != std::string_view::npos ||
        value.find('\\') != std::string_view::npos) {
        return true;
    }
    return value.size() > 1U && value.front() == '-' &&
           !(value[1U] >= '0' && value[1U] <= '9');
}

bool word_scalar_sequence(const std::vector<FrontendScalar> &scalars,
                          std::size_t begin,
                          std::size_t end) noexcept {
    if (begin >= end) {
        return false;
    }
    for (std::size_t index = begin; index < end; ++index) {
        const std::uint32_t scalar = scalars[index].value;
        if (latin_letter(scalar)) {
            continue;
        }
        const bool apostrophe = (scalar == 0x0027U || scalar == 0x2019U) &&
                                index > begin && index + 1U < end &&
                                latin_letter(scalars[index - 1U].value) &&
                                latin_letter(scalars[index + 1U].value);
        if (!apostrophe) {
            return false;
        }
    }
    return true;
}

bool prose_atomic_value(std::string_view value,
                        const std::vector<FrontendScalar> &scalars,
                        std::size_t begin,
                        std::size_t end) noexcept {
    if (value.empty()) {
        return false;
    }
    if (word_scalar_sequence(scalars, begin, end)) {
        return true;
    }
    const unsigned char first = static_cast<unsigned char>(value.front());
    if ((first >= static_cast<unsigned char>('0') &&
         first <= static_cast<unsigned char>('9')) ||
        (first == static_cast<unsigned char>('$') && value.size() > 1U &&
         value[1U] >= '0' && value[1U] <= '9') ||
        (value.size() > 1U && (value.front() == '+' || value.front() == '-') &&
         value[1U] >= '0' && value[1U] <= '9') ||
        (value.rfind("\xc2\xa3", 0U) == 0U && value.size() > 2U &&
         value[2U] >= '0' && value[2U] <= '9') ||
        (value.rfind("\xe2\x82\xac", 0U) == 0U && value.size() > 3U &&
         value[3U] >= '0' && value[3U] <= '9')) {
        return true;
    }
    constexpr std::array<std::string_view, 15> units = {
        "Hz", "kHz", "MHz", "GHz", "B", "KiB", "MiB", "GiB",
        "km", "m", "cm", "mm", "kg", "g", "%",
    };
    return std::find(units.begin(), units.end(), value) != units.end();
}

bool prose_wrapper(std::uint32_t scalar) noexcept {
    return scalar == 0x0022U || scalar == 0x0027U ||
           scalar == 0x0028U || scalar == 0x0029U ||
           scalar == 0x005bU || scalar == 0x005dU ||
           scalar == 0x007bU || scalar == 0x007dU ||
           scalar == 0x2018U || scalar == 0x2019U ||
           scalar == 0x201cU || scalar == 0x201dU;
}

int process_prose_core(const std::vector<FrontendScalar> &scalars,
                       std::size_t begin,
                       std::size_t end,
                       Builder *builder) {
    if (begin >= end) {
        return KGV_OK;
    }
    const std::string value = scalar_text(builder->text, scalars, begin, end);
    const SourceSpan span = scalar_span(scalars, begin, end);
    if (dotted_initialism(value)) {
        spell_letters(value, builder, span);
        return KGV_OK;
    }
    if (prose_atomic_value(value, scalars, begin, end)) {
        return process_core(value, scalars, begin, end, builder);
    }
    if (prose_protected_atom(value)) {
        expand_terminal_atom(value, builder, span);
        return KGV_OK;
    }

    std::size_t index = begin;
    while (index < end) {
        const std::uint32_t scalar = scalars[index].value;
        if (latin_letter(scalar)) {
            const std::size_t run_begin = index;
            ++index;
            while (index < end) {
                if (latin_letter(scalars[index].value)) {
                    ++index;
                    continue;
                }
                const bool contraction =
                    (scalars[index].value == 0x0027U ||
                     scalars[index].value == 0x2019U) &&
                    index + 1U < end && latin_letter(scalars[index + 1U].value);
                if (!contraction) {
                    break;
                }
                ++index;
            }
            const std::string run = scalar_text(builder->text, scalars, run_begin, index);
            const int status = expand_word(run, scalar_span(scalars, run_begin, index),
                                           builder);
            if (status != KGV_OK) {
                return status;
            }
            continue;
        }
        if (scalar >= static_cast<std::uint32_t>('0') &&
            scalar <= static_cast<std::uint32_t>('9')) {
            const std::size_t run_begin = index;
            while (index < end && scalars[index].value >=
                                      static_cast<std::uint32_t>('0') &&
                   scalars[index].value <= static_cast<std::uint32_t>('9')) {
                ++index;
            }
            const std::string run = scalar_text(builder->text, scalars, run_begin, index);
            (void)expand_integer(run, builder, scalar_span(scalars, run_begin, index));
            continue;
        }
        const SourceSpan scalar_source = scalar_span(scalars, index, index + 1U);
        if (scalar == 0x2013U || scalar == 0x2014U) {
            builder->end_phrase(PhraseTerminator::comma, scalar_source);
            ++index;
            continue;
        }
        if (scalar <= 0x7fU) {
            const char byte = static_cast<char>(scalar);
            const PhraseTerminator terminator = punctuation_terminator(byte);
            if (terminator != PhraseTerminator::none) {
                builder->end_phrase(terminator, scalar_source);
                ++index;
                continue;
            }
            if (prose_wrapper(scalar) || byte == '-') {
                ++index;
                continue;
            }
            const std::string_view symbol = terminal_symbol_name(byte);
            if (!symbol.empty()) {
                add_terminal_symbol(byte, builder, scalar_source);
                ++index;
                continue;
            }
        } else if (prose_wrapper(scalar)) {
            ++index;
            continue;
        }
        return builder->reject("UNSUPPORTED_SCALAR",
                               "scalar is outside the v1 prose inventory",
                               scalar_source);
    }
    return KGV_OK;
}

int validate_punctuation_depth(const std::vector<FrontendScalar> &scalars,
                               Builder *builder) {
    std::size_t depth = 0U;
    for (const FrontendScalar &scalar : scalars) {
        const bool opening = scalar.value == 0x0028U || scalar.value == 0x005bU ||
                             scalar.value == 0x007bU;
        const bool closing = scalar.value == 0x0029U || scalar.value == 0x005dU ||
                             scalar.value == 0x007dU;
        if (opening) {
            ++depth;
            if (depth > kMaximumNestingDepth) {
                return builder->reject(
                    "NESTING_TOO_DEEP",
                    "punctuation nesting exceeds the 128-level limit",
                    SourceSpan{scalar.byte_start, scalar.byte_end});
            }
        } else if (closing && depth > 0U) {
            --depth;
        }
    }
    return KGV_OK;
}

std::string status_name(int status) {
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

void json_string(std::ostringstream *stream, std::string_view value) {
    *stream << '"';
    constexpr char hexadecimal[] = "0123456789abcdef";
    for (char raw_byte : value) {
        const auto byte = static_cast<unsigned char>(raw_byte);
        switch (byte) {
            case '"': *stream << "\\\""; break;
            case '\\': *stream << "\\\\"; break;
            case '\b': *stream << "\\b"; break;
            case '\f': *stream << "\\f"; break;
            case '\n': *stream << "\\n"; break;
            case '\r': *stream << "\\r"; break;
            case '\t': *stream << "\\t"; break;
            default:
                if (byte < 0x20U) {
                    *stream << "\\u00" << hexadecimal[byte >> 4U]
                            << hexadecimal[byte & 0x0fU];
                } else {
                    *stream << static_cast<char>(byte);
                }
                break;
        }
    }
    *stream << '"';
}

void json_span(std::ostringstream *stream, SourceSpan span) {
    *stream << "{\"byte_end\":" << span.byte_end
            << ",\"byte_start\":" << span.byte_start << '}';
}

}  // namespace

const char *phrase_terminator_name(PhraseTerminator value) noexcept {
    switch (value) {
        case PhraseTerminator::none: return "END_NONE";
        case PhraseTerminator::comma: return "END_COMMA";
        case PhraseTerminator::colon: return "END_COLON";
        case PhraseTerminator::semicolon: return "END_SEMICOLON";
        case PhraseTerminator::period: return "END_PERIOD";
        case PhraseTerminator::question: return "END_QUESTION";
        case PhraseTerminator::exclamation: return "END_EXCLAMATION";
        case PhraseTerminator::paragraph: return "END_PARAGRAPH";
        case PhraseTerminator::continuation: return "END_CONTINUATION";
    }
    return "END_NONE";
}

int run_lexical_frontend(std::string_view text,
                         std::uint32_t profile,
                         LexicalFrontendResult *result,
                         FrontendFailure *failure) {
    if (result == nullptr || failure == nullptr) {
        return KGV_INVALID_ARGUMENT;
    }
    *result = LexicalFrontendResult{};
    *failure = FrontendFailure{};
    result->profile = profile == KGV_PROFILE_TERMINAL ? "terminal" : "prose";
    result->input_bytes = text.size();

    FrontendAnalysis analysis;
    const int validation_status = analyze_frontend(text, profile, &analysis, failure);
    result->ignored_control_sequences = analysis.ignored_control_sequences;
    if (validation_status != KGV_OK) {
        if (!failure->code.empty()) {
            result->diagnostics.push_back(FrontendDiagnostic{
                failure->code,
                "ERROR",
                SourceSpan{failure->byte_offset,
                           std::min(text.size(), failure->byte_offset + 1U)},
            });
        }
        return validation_status;
    }

    for (const FrontendControlSequence &sequence : analysis.control_sequences) {
        result->diagnostics.push_back(FrontendDiagnostic{
            "CONTROL_SEQUENCE_IGNORED",
            "INFO",
            SourceSpan{sequence.byte_start, sequence.byte_end},
        });
    }

    Builder builder(text, profile, result, failure);
    std::vector<FrontendScalar> normalized_scalars;
    const int normalization_status = normalize_frontend_nfc(
        text, analysis.visible_scalars, &normalized_scalars, failure);
    if (normalization_status != KGV_OK) {
        result->diagnostics.push_back(FrontendDiagnostic{
            failure->code,
            "ERROR",
            SourceSpan{failure->byte_offset,
                       std::min(text.size(), failure->byte_offset + 1U)},
        });
        return normalization_status;
    }
    const std::vector<FrontendScalar> &scalars = normalized_scalars;
    const auto fail_closed = [&](int status) {
        result->words.clear();
        result->phrases.clear();
        return status;
    };
    const int nesting_status = validate_punctuation_depth(scalars, &builder);
    if (nesting_status != KGV_OK) {
        return fail_closed(nesting_status);
    }
    std::size_t index = 0U;
    std::size_t consecutive_line_breaks = 0U;
    while (index < scalars.size()) {
        if (whitespace(scalars[index].value)) {
            const std::size_t whitespace_begin = index;
            while (index < scalars.size() && whitespace(scalars[index].value)) {
                if (line_break(scalars[index].value)) {
                    if (scalars[index].value == 0x000dU && index + 1U < scalars.size() &&
                        scalars[index + 1U].value == 0x000aU) {
                        ++index;
                    }
                    ++consecutive_line_breaks;
                }
                ++index;
            }
            const SourceSpan span = scalar_span(scalars, whitespace_begin, index);
            if (consecutive_line_breaks >= 2U) {
                builder.end_phrase(PhraseTerminator::paragraph, span, true);
            } else if (consecutive_line_breaks == 1U && profile == KGV_PROFILE_TERMINAL) {
                builder.end_phrase(PhraseTerminator::none, span);
            }
            consecutive_line_breaks = 0U;
            continue;
        }

        const std::size_t atom_begin = index;
        while (index < scalars.size() && !whitespace(scalars[index].value)) {
            ++index;
        }
        const std::size_t atom_end = index;
        const SourceSpan atom_span = scalar_span(scalars, atom_begin, atom_end);
        if (atom_end - atom_begin > kMaximumSurfaceScalars ||
            atom_span.byte_end - atom_span.byte_start > kMaximumSurfaceBytes) {
            const SourceSpan span = atom_span;
            failure->status = KGV_INPUT_TOO_LARGE;
            failure->code = "SURFACE_TOKEN_TOO_LARGE";
            failure->message = "surface token exceeds the 4096-byte/scalar limit";
            failure->byte_offset = span.byte_start;
            result->diagnostics.push_back(
                FrontendDiagnostic{failure->code, "ERROR", span});
            return fail_closed(KGV_INPUT_TOO_LARGE);
        }

        std::string atom = scalar_text(text, scalars, atom_begin, atom_end);
        if (atom == "Dr.") {
            const int status = expand_word(atom, atom_span, &builder);
            if (status != KGV_OK) {
                return fail_closed(status);
            }
            continue;
        }
        if (profile == KGV_PROFILE_TERMINAL) {
            const int status = process_core(atom, scalars, atom_begin, atom_end, &builder);
            if (status != KGV_OK) {
                return fail_closed(status);
            }
            continue;
        }

        std::size_t content_end = atom_end;
        while (content_end > atom_begin &&
               (scalars[content_end - 1U].value == 0x0022U ||
                scalars[content_end - 1U].value == 0x2019U ||
                scalars[content_end - 1U].value == 0x201dU)) {
            --content_end;
        }
        std::size_t core_end = content_end;
        while (core_end > atom_begin && scalars[core_end - 1U].value <= 0x7fU &&
               punctuation_terminator(
                   static_cast<char>(scalars[core_end - 1U].value)) !=
                   PhraseTerminator::none) {
            --core_end;
        }
        const int core_status = process_prose_core(scalars, atom_begin, core_end, &builder);
        if (core_status != KGV_OK) {
            return fail_closed(core_status);
        }
        std::size_t punctuation_begin = core_end;
        const std::string core_value = scalar_text(text, scalars, atom_begin, core_end);
        if (dotted_initialism(core_value) && content_end - core_end == 1U &&
            scalars[core_end].value == static_cast<std::uint32_t>('.')) {
            std::size_t lookahead = index;
            while (lookahead < scalars.size() && whitespace(scalars[lookahead].value)) {
                ++lookahead;
            }
            if (lookahead < scalars.size() &&
                scalars[lookahead].value >= static_cast<std::uint32_t>('a') &&
                scalars[lookahead].value <= static_cast<std::uint32_t>('z')) {
                ++punctuation_begin;
            }
        }
        for (std::size_t punctuation = punctuation_begin;
             punctuation < content_end; ++punctuation) {
            const PhraseTerminator terminator = punctuation_terminator(
                static_cast<char>(scalars[punctuation].value));
            if (terminator != PhraseTerminator::none) {
                builder.end_phrase(terminator,
                                   scalar_span(scalars, punctuation, punctuation + 1U));
            }
        }
    }
    builder.finish();
    return KGV_OK;
}

std::string lexical_frontend_json(const LexicalFrontendResult &result,
                                  int status,
                                  const FrontendFailure &failure) {
    std::ostringstream stream;
    stream << "{\"diagnostics\":[";
    for (std::size_t index = 0U; index < result.diagnostics.size(); ++index) {
        if (index > 0U) stream << ',';
        const FrontendDiagnostic &diagnostic = result.diagnostics[index];
        stream << "{\"code\":";
        json_string(&stream, diagnostic.code);
        stream << ",\"severity\":";
        json_string(&stream, diagnostic.severity);
        stream << ",\"span\":";
        json_span(&stream, diagnostic.span);
        stream << '}';
    }
    stream << "],\"dialect\":\"en-AU\",\"ignored_control_sequences\":"
           << result.ignored_control_sequences << ",\"input_bytes\":"
           << result.input_bytes << ",\"phrases\":[";
    for (std::size_t index = 0U; index < result.phrases.size(); ++index) {
        if (index > 0U) stream << ',';
        const LexicalPhrase &phrase = result.phrases[index];
        stream << "{\"span\":";
        json_span(&stream, phrase.span);
        stream << ",\"terminator\":";
        json_string(&stream, phrase_terminator_name(phrase.terminator));
        stream << ",\"word_end\":" << phrase.word_end
               << ",\"word_start\":" << phrase.word_start << '}';
    }
    stream << "],\"profile\":";
    json_string(&stream, result.profile);
    stream << ",\"schema\":\"kilix.voicegen.frontend-lexical-trace/v1\",\"status\":";
    json_string(&stream, status_name(status));
    if (status != KGV_OK && !failure.code.empty()) {
        stream << ",\"failure_code\":";
        json_string(&stream, failure.code);
        stream << ",\"failure_byte_offset\":" << failure.byte_offset;
    }
    stream << ",\"words\":[";
    for (std::size_t index = 0U; index < result.words.size(); ++index) {
        if (index > 0U) stream << ',';
        const LexicalWord &word = result.words[index];
        stream << "{\"normalized\":";
        json_string(&stream, word.normalized);
        stream << ",\"source_kind\":";
        json_string(&stream, word.source_kind);
        stream << ",\"span\":";
        json_span(&stream, word.span);
        stream << '}';
    }
    stream << "]}\n";
    return stream.str();
}

}  // namespace kgv
