#include <algorithm>
#include <iostream>
#include <string>

#include "runtime/sha256.h"

namespace {

bool expect(std::string_view input, std::string_view expected) {
    const std::string actual = kgv::sha256_hex(input);
    if (actual != expected) {
        std::cerr << "SHA-256 mismatch: expected " << expected << ", got " << actual << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main() {
    if (!expect("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") ||
        !expect("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") ||
        !expect("The quick brown fox jumps over the lazy dog",
                "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592")) {
        return 1;
    }

    kgv::Sha256 incremental;
    const std::string million_a(1000000U, 'a');
    for (std::size_t offset = 0U; offset < million_a.size(); offset += 137U) {
        const std::size_t amount = std::min<std::size_t>(137U, million_a.size() - offset);
        incremental.update(million_a.data() + static_cast<std::ptrdiff_t>(offset), amount);
    }
    const std::string actual = kgv::sha256_hex(incremental.finish());
    if (actual != "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") {
        std::cerr << "incremental SHA-256 mismatch\n";
        return 1;
    }
    if (!kgv::is_lower_sha256(actual) || kgv::is_lower_sha256("ABC") ||
        kgv::is_lower_sha256(std::string(64U, 'A'))) {
        std::cerr << "SHA-256 lexical validator mismatch\n";
        return 1;
    }
    return 0;
}
