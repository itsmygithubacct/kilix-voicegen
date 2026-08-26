#ifndef KGV_RUNTIME_PIPER_RUNTIME_H
#define KGV_RUNTIME_PIPER_RUNTIME_H

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "kilix_voicegen.h"
#include "runtime/model_package.h"
#include "runtime/piper_projection.h"

namespace kgv {

class PiperRuntime final {
public:
    ~PiperRuntime();
    PiperRuntime(const PiperRuntime &) = delete;
    PiperRuntime &operator=(const PiperRuntime &) = delete;

    static int create(const VerifiedModel &model,
                      std::uint32_t thread_count,
                      std::unique_ptr<PiperRuntime> *runtime,
                      std::string *error);

    int synthesize(const std::vector<PiperProjectedChunk> &chunks,
                   float rate,
                   std::atomic<bool> *cancelled,
                   kgv_pcm_callback callback,
                   void *user,
                   std::string *error);

    void cancel_active() noexcept;

private:
    struct Impl;
    explicit PiperRuntime(std::unique_ptr<Impl> implementation);
    std::unique_ptr<Impl> implementation_;
};

}  // namespace kgv

#endif
