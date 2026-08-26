#ifndef KGV_RUNTIME_JSON_H
#define KGV_RUNTIME_JSON_H

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace kgv::json {

struct Number final {
    std::string lexical;
};

class Value final {
public:
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value, std::less<>>;
    using Storage = std::variant<std::nullptr_t, bool, Number, std::string, Array, Object>;

    explicit Value(std::nullptr_t value = nullptr);
    explicit Value(bool value);
    explicit Value(Number value);
    explicit Value(std::string value);
    explicit Value(Array value);
    explicit Value(Object value);

    bool is_null() const noexcept;
    bool is_bool() const noexcept;
    bool is_number() const noexcept;
    bool is_string() const noexcept;
    bool is_array() const noexcept;
    bool is_object() const noexcept;

    bool as_bool() const;
    std::int64_t as_integer() const;
    const std::string &as_string() const;
    const Array &as_array() const;
    const Object &as_object() const;
    const Value *find(std::string_view key) const;

private:
    Storage storage_;
};

class ParseError final : public std::runtime_error {
public:
    ParseError(std::size_t offset, const std::string &message);
    std::size_t offset() const noexcept;

private:
    std::size_t offset_;
};

Value parse(std::string_view input);

}  // namespace kgv::json

#endif
