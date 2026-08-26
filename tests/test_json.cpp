#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "runtime/json.h"

namespace {

bool rejects(std::string_view input) {
    try {
        (void)kgv::json::parse(input);
        return false;
    } catch (const kgv::json::ParseError &) {
        return true;
    }
}

}  // namespace

int main() {
    const kgv::json::Value document = kgv::json::parse(
        R"({"array":[true,false,null,-7],"emoji":"\ud83d\ude00","name":"fixture"})");
    if (!document.is_object() || document.find("missing") != nullptr ||
        document.find("name") == nullptr || document.find("name")->as_string() != "fixture" ||
        document.find("emoji") == nullptr || document.find("emoji")->as_string() != "\xf0\x9f\x98\x80") {
        std::cerr << "valid JSON document parsed incorrectly\n";
        return 1;
    }
    const auto *array = document.find("array");
    if (array == nullptr || array->as_array().size() != 4U ||
        !array->as_array()[0].as_bool() || array->as_array()[1].as_bool() ||
        !array->as_array()[2].is_null() || array->as_array()[3].as_integer() != -7) {
        std::cerr << "valid JSON values parsed incorrectly\n";
        return 1;
    }

    const std::string invalid_utf8 = std::string("{\"x\":\"") +
                                     static_cast<char>(0xc3) + "(\"}";
    const std::string deeply_nested(66U, '[');
    const std::string deeply_closed(66U, ']');
    const std::string cases[] = {
        "{\"x\":1,\"x\":2}",
        "{\"x\":\"\\ud800\"}",
        "{\"x\":\"\\udc00\"}",
        "{\"x\":01}",
        "{\"x\":1.}",
        "{\"x\":true} trailing",
        invalid_utf8,
        deeply_nested + "0" + deeply_closed,
    };
    for (const auto &input : cases) {
        if (!rejects(input)) {
            std::cerr << "strict JSON parser accepted an invalid document\n";
            return 1;
        }
    }

    const kgv::json::Value floating = kgv::json::parse("1.25e2");
    try {
        (void)floating.as_integer();
        std::cerr << "fractional JSON number was exposed as an integer\n";
        return 1;
    } catch (const std::runtime_error &) {
    }
    return 0;
}
