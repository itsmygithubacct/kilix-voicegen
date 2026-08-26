#include "runtime/json.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace kgv::json {
namespace {

bool is_continuation(unsigned char byte) noexcept {
    return (byte & 0xc0U) == 0x80U;
}

bool valid_utf8(std::string_view value) noexcept {
    std::size_t offset = 0U;
    while (offset < value.size()) {
        const auto first = static_cast<unsigned char>(value[offset]);
        if (first <= 0x7fU) {
            ++offset;
            continue;
        }
        if (first >= 0xc2U && first <= 0xdfU) {
            if (offset + 1U >= value.size() ||
                !is_continuation(static_cast<unsigned char>(value[offset + 1U]))) {
                return false;
            }
            offset += 2U;
            continue;
        }
        if (first >= 0xe0U && first <= 0xefU) {
            if (offset + 2U >= value.size()) {
                return false;
            }
            const auto second = static_cast<unsigned char>(value[offset + 1U]);
            const auto third = static_cast<unsigned char>(value[offset + 2U]);
            if (!is_continuation(second) || !is_continuation(third) ||
                (first == 0xe0U && second < 0xa0U) ||
                (first == 0xedU && second >= 0xa0U)) {
                return false;
            }
            offset += 3U;
            continue;
        }
        if (first >= 0xf0U && first <= 0xf4U) {
            if (offset + 3U >= value.size()) {
                return false;
            }
            const auto second = static_cast<unsigned char>(value[offset + 1U]);
            const auto third = static_cast<unsigned char>(value[offset + 2U]);
            const auto fourth = static_cast<unsigned char>(value[offset + 3U]);
            if (!is_continuation(second) || !is_continuation(third) ||
                !is_continuation(fourth) ||
                (first == 0xf0U && second < 0x90U) ||
                (first == 0xf4U && second >= 0x90U)) {
                return false;
            }
            offset += 4U;
            continue;
        }
        return false;
    }
    return true;
}

int hex_value(char character) noexcept {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return 10 + character - 'a';
    }
    if (character >= 'A' && character <= 'F') {
        return 10 + character - 'A';
    }
    return -1;
}

void append_utf8(std::string *output, std::uint32_t scalar) {
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

class Parser final {
public:
    explicit Parser(std::string_view input) : input_(input) {}

    Value parse_document() {
        skip_whitespace();
        Value result = parse_value(0U);
        skip_whitespace();
        if (position_ != input_.size()) {
            fail("trailing data after JSON value");
        }
        return result;
    }

private:
    [[noreturn]] void fail(const std::string &message) const {
        throw ParseError(position_, message);
    }

    bool consume(char expected) {
        if (position_ < input_.size() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void expect(char expected) {
        if (!consume(expected)) {
            fail("unexpected JSON character");
        }
    }

    void skip_whitespace() {
        while (position_ < input_.size()) {
            const char current = input_[position_];
            if (current != ' ' && current != '\t' && current != '\n' && current != '\r') {
                break;
            }
            ++position_;
        }
    }

    Value parse_value(std::size_t depth) {
        if (depth > 64U) {
            fail("JSON nesting limit exceeded");
        }
        if (position_ >= input_.size()) {
            fail("unexpected end of JSON input");
        }
        switch (input_[position_]) {
        case 'n':
            parse_keyword("null");
            return Value(nullptr);
        case 't':
            parse_keyword("true");
            return Value(true);
        case 'f':
            parse_keyword("false");
            return Value(false);
        case '"':
            return Value(parse_string());
        case '[':
            return Value(parse_array(depth + 1U));
        case '{':
            return Value(parse_object(depth + 1U));
        default:
            if (input_[position_] == '-' ||
                (input_[position_] >= '0' && input_[position_] <= '9')) {
                return Value(Number{parse_number()});
            }
            fail("invalid JSON value");
        }
    }

    void parse_keyword(std::string_view keyword) {
        if (input_.substr(position_, keyword.size()) != keyword) {
            fail("invalid JSON keyword");
        }
        position_ += keyword.size();
    }

    std::uint16_t parse_hex_quad() {
        if (input_.size() - position_ < 4U) {
            fail("truncated JSON Unicode escape");
        }
        std::uint16_t value = 0U;
        for (std::size_t i = 0U; i < 4U; ++i) {
            const int digit = hex_value(input_[position_ + i]);
            if (digit < 0) {
                fail("invalid JSON Unicode escape");
            }
            value = static_cast<std::uint16_t>(
                (static_cast<std::uint32_t>(value) << 4U) |
                static_cast<std::uint32_t>(digit));
        }
        position_ += 4U;
        return value;
    }

    std::string parse_string() {
        expect('"');
        std::string output;
        while (position_ < input_.size()) {
            const auto byte = static_cast<unsigned char>(input_[position_++]);
            if (byte == static_cast<unsigned char>('"')) {
                if (!valid_utf8(output)) {
                    fail("JSON string is not valid UTF-8");
                }
                return output;
            }
            if (byte < 0x20U) {
                fail("unescaped control in JSON string");
            }
            if (byte != static_cast<unsigned char>('\\')) {
                output.push_back(static_cast<char>(byte));
                continue;
            }
            if (position_ >= input_.size()) {
                fail("truncated JSON escape");
            }
            const char escaped = input_[position_++];
            switch (escaped) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                const std::uint16_t first = parse_hex_quad();
                std::uint32_t scalar = first;
                if (first >= 0xd800U && first <= 0xdbffU) {
                    if (!consume('\\') || !consume('u')) {
                        fail("JSON high surrogate has no low surrogate");
                    }
                    const std::uint16_t second = parse_hex_quad();
                    if (second < 0xdc00U || second > 0xdfffU) {
                        fail("invalid JSON low surrogate");
                    }
                    scalar = 0x10000U +
                             ((static_cast<std::uint32_t>(first) - 0xd800U) << 10U) +
                             (static_cast<std::uint32_t>(second) - 0xdc00U);
                } else if (first >= 0xdc00U && first <= 0xdfffU) {
                    fail("unpaired JSON low surrogate");
                }
                append_utf8(&output, scalar);
                break;
            }
            default:
                fail("unknown JSON escape");
            }
        }
        fail("unterminated JSON string");
    }

    std::string parse_number() {
        const std::size_t begin = position_;
        consume('-');
        if (position_ >= input_.size()) {
            fail("truncated JSON number");
        }
        if (consume('0')) {
            if (position_ < input_.size() && input_[position_] >= '0' &&
                input_[position_] <= '9') {
                fail("leading zero in JSON number");
            }
        } else {
            if (input_[position_] < '1' || input_[position_] > '9') {
                fail("invalid JSON number");
            }
            do {
                ++position_;
            } while (position_ < input_.size() && input_[position_] >= '0' &&
                     input_[position_] <= '9');
        }
        if (consume('.')) {
            if (position_ >= input_.size() || input_[position_] < '0' ||
                input_[position_] > '9') {
                fail("invalid JSON number fraction");
            }
            do {
                ++position_;
            } while (position_ < input_.size() && input_[position_] >= '0' &&
                     input_[position_] <= '9');
        }
        if (position_ < input_.size() &&
            (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            if (position_ >= input_.size() || input_[position_] < '0' ||
                input_[position_] > '9') {
                fail("invalid JSON number exponent");
            }
            do {
                ++position_;
            } while (position_ < input_.size() && input_[position_] >= '0' &&
                     input_[position_] <= '9');
        }
        return std::string(input_.substr(begin, position_ - begin));
    }

    Value::Array parse_array(std::size_t depth) {
        expect('[');
        skip_whitespace();
        Value::Array result;
        if (consume(']')) {
            return result;
        }
        while (true) {
            skip_whitespace();
            result.push_back(parse_value(depth));
            skip_whitespace();
            if (consume(']')) {
                return result;
            }
            expect(',');
            skip_whitespace();
        }
    }

    Value::Object parse_object(std::size_t depth) {
        expect('{');
        skip_whitespace();
        Value::Object result;
        if (consume('}')) {
            return result;
        }
        while (true) {
            skip_whitespace();
            if (position_ >= input_.size() || input_[position_] != '"') {
                fail("JSON object key is not a string");
            }
            std::string key = parse_string();
            skip_whitespace();
            expect(':');
            skip_whitespace();
            auto inserted = result.emplace(std::move(key), parse_value(depth));
            if (!inserted.second) {
                fail("duplicate JSON object key");
            }
            skip_whitespace();
            if (consume('}')) {
                return result;
            }
            expect(',');
            skip_whitespace();
        }
    }

    std::string_view input_;
    std::size_t position_ = 0U;
};

}  // namespace

Value::Value(std::nullptr_t value) : storage_(value) {}
Value::Value(bool value) : storage_(value) {}
Value::Value(Number value) : storage_(std::move(value)) {}
Value::Value(std::string value) : storage_(std::move(value)) {}
Value::Value(Array value) : storage_(std::move(value)) {}
Value::Value(Object value) : storage_(std::move(value)) {}

bool Value::is_null() const noexcept { return std::holds_alternative<std::nullptr_t>(storage_); }
bool Value::is_bool() const noexcept { return std::holds_alternative<bool>(storage_); }
bool Value::is_number() const noexcept { return std::holds_alternative<Number>(storage_); }
bool Value::is_string() const noexcept { return std::holds_alternative<std::string>(storage_); }
bool Value::is_array() const noexcept { return std::holds_alternative<Array>(storage_); }
bool Value::is_object() const noexcept { return std::holds_alternative<Object>(storage_); }

bool Value::as_bool() const {
    if (!is_bool()) {
        throw std::runtime_error("JSON value is not a boolean");
    }
    return std::get<bool>(storage_);
}

std::int64_t Value::as_integer() const {
    if (!is_number()) {
        throw std::runtime_error("JSON value is not a number");
    }
    const std::string &lexical = std::get<Number>(storage_).lexical;
    if (lexical.find_first_of(".eE") != std::string::npos) {
        throw std::runtime_error("JSON number is not an integer");
    }
    std::int64_t result = 0;
    const auto converted = std::from_chars(lexical.data(),
                                           lexical.data() + lexical.size(), result);
    if (converted.ec != std::errc{} || converted.ptr != lexical.data() + lexical.size()) {
        throw std::runtime_error("JSON integer is outside the signed 64-bit range");
    }
    return result;
}

const std::string &Value::as_string() const {
    if (!is_string()) {
        throw std::runtime_error("JSON value is not a string");
    }
    return std::get<std::string>(storage_);
}

const Value::Array &Value::as_array() const {
    if (!is_array()) {
        throw std::runtime_error("JSON value is not an array");
    }
    return std::get<Array>(storage_);
}

const Value::Object &Value::as_object() const {
    if (!is_object()) {
        throw std::runtime_error("JSON value is not an object");
    }
    return std::get<Object>(storage_);
}

const Value *Value::find(std::string_view key) const {
    if (!is_object()) {
        return nullptr;
    }
    const Object &object = std::get<Object>(storage_);
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->second;
}

ParseError::ParseError(std::size_t offset, const std::string &message)
    : std::runtime_error(message), offset_(offset) {}

std::size_t ParseError::offset() const noexcept { return offset_; }

Value parse(std::string_view input) {
    return Parser(input).parse_document();
}

}  // namespace kgv::json
