#ifndef KGV_RUNTIME_PIPER_PROJECTION_H
#define KGV_RUNTIME_PIPER_PROJECTION_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "frontend/tokenization.h"

namespace kgv {

struct PiperProjectedChunk final {
    std::vector<std::int64_t> ids;
    SourceSpan source_span;
    bool continuation = false;
};

class PiperTokenProjection final {
public:
    const std::string &resource_id() const noexcept;
    const std::string &resource_sha256() const noexcept;
    const std::string &source_token_inventory_sha256() const noexcept;
    std::size_t maximum_output_tokens() const noexcept;
    std::uint16_t target_id_max() const noexcept;

private:
    friend int load_piper_token_projection(
        std::string_view,
        std::string_view,
        const ModelTokenInventory &,
        std::uint16_t,
        PiperTokenProjection *,
        std::string *);
    friend int project_piper_tokens(
        const PiperTokenProjection &,
        const ModelTokenResult &,
        std::vector<PiperProjectedChunk> *,
        std::string *);

    std::string resource_id_;
    std::string resource_sha256_;
    std::string source_token_inventory_sha256_;
    std::size_t maximum_output_tokens_ = 0U;
    std::uint16_t target_id_max_ = 0U;
    std::map<std::uint16_t, std::vector<std::int64_t>> entries_;
};

int load_piper_token_projection(
    std::string_view jsonl,
    std::string_view expected_resource_sha256,
    const ModelTokenInventory &source_inventory,
    std::uint16_t expected_target_id_max,
    PiperTokenProjection *projection,
    std::string *error);

int project_piper_tokens(
    const PiperTokenProjection &projection,
    const ModelTokenResult &source,
    std::vector<PiperProjectedChunk> *chunks,
    std::string *error);

}  // namespace kgv

#endif
