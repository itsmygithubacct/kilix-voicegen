#include "frontend/pipeline.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

constexpr std::uint64_t kMaximumP95Microseconds = 10000U;
constexpr std::uint64_t kMaximumResourceAndWorkingKib = 32768U;

bool run_frontend(const std::string &input) {
    kgv::LexicalFrontendResult result;
    kgv::FrontendFailure failure;
    return kgv::run_lexical_frontend(
               input, KGV_PROFILE_PROSE, &result, &failure) == KGV_OK &&
           !result.words.empty();
}

std::uint64_t current_rss_kib() {
    std::ifstream stream("/proc/self/statm");
    std::uint64_t virtual_pages = 0U;
    std::uint64_t resident_pages = 0U;
    if (!(stream >> virtual_pages >> resident_pages)) {
        return 0U;
    }
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0L) {
        return 0U;
    }
    return resident_pages * static_cast<std::uint64_t>(page_size) / 1024U;
}

}  // namespace

int main() {
    std::string maximum_request;
    maximum_request.reserve(KGV_MAX_INPUT_BYTES);
    while (maximum_request.size() + 5U <= KGV_MAX_INPUT_BYTES) {
        maximum_request.append("word ");
    }
    maximum_request.append(KGV_MAX_INPUT_BYTES - maximum_request.size(), ' ');
    if (maximum_request.size() != KGV_MAX_INPUT_BYTES) {
        std::cerr << "could not process the 64 KiB benchmark request\n";
        return 1;
    }

    const std::uint64_t baseline_rss_kib = current_rss_kib();
    if (baseline_rss_kib == 0U) {
        std::cerr << "could not measure baseline process RSS\n";
        return 1;
    }
    std::atomic<bool> sampling{true};
    std::atomic<bool> sampler_ready{false};
    std::atomic<std::uint64_t> peak_rss_kib{baseline_rss_kib};
    std::thread sampler([&]() {
        sampler_ready.store(true, std::memory_order_release);
        while (sampling.load(std::memory_order_acquire)) {
            const std::uint64_t measured = current_rss_kib();
            std::uint64_t observed = peak_rss_kib.load(std::memory_order_relaxed);
            while (measured > observed &&
                   !peak_rss_kib.compare_exchange_weak(
                       observed, measured, std::memory_order_relaxed)) {
            }
            std::this_thread::yield();
        }
    });
    while (!sampler_ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    kgv::LexicalFrontendResult maximum_result;
    kgv::FrontendFailure maximum_failure;
    const int maximum_status = kgv::run_lexical_frontend(
        maximum_request, KGV_PROFILE_PROSE, &maximum_result, &maximum_failure);
    sampling.store(false, std::memory_order_release);
    sampler.join();
    if (maximum_status != KGV_OK || maximum_result.words.empty()) {
        std::cerr << "could not process the 64 KiB benchmark request\n";
        return 1;
    }
    const std::uint64_t measured_peak_rss_kib =
        std::max(peak_rss_kib.load(), current_rss_kib());
    const std::uint64_t resource_and_working_kib =
        measured_peak_rss_kib > baseline_rss_kib
            ? measured_peak_rss_kib - baseline_rss_kib
            : 0U;

    std::string latency_request;
    latency_request.reserve(1024U);
    while (latency_request.size() + 5U <= 1024U) {
        latency_request.append("word ");
    }
    latency_request.append(1024U - latency_request.size(), ' ');
    for (std::size_t warmup = 0U; warmup < 100U; ++warmup) {
        if (!run_frontend(latency_request)) {
            std::cerr << "could not process the latency benchmark request\n";
            return 1;
        }
    }

    std::vector<std::uint64_t> microseconds;
    microseconds.reserve(1000U);
    for (std::size_t iteration = 0U; iteration < 1000U; ++iteration) {
        const auto begin = std::chrono::steady_clock::now();
        const bool succeeded = run_frontend(latency_request);
        const auto end = std::chrono::steady_clock::now();
        if (!succeeded) {
            std::cerr << "latency benchmark request failed\n";
            return 1;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            end - begin);
        microseconds.push_back(static_cast<std::uint64_t>(elapsed.count()));
    }
    std::sort(microseconds.begin(), microseconds.end());
    const std::uint64_t median = microseconds[499U];
    const std::uint64_t p95 = microseconds[949U];
    const std::uint64_t p99 = microseconds[989U];
    std::cout << "{\"iterations\":1000,\"maximum_request_bytes\":65536,"
                 "\"baseline_process_rss_kib\":"
              << baseline_rss_kib << ",\"peak_process_rss_kib\":"
              << measured_peak_rss_kib << ",\"resource_and_working_rss_kib\":"
              << resource_and_working_kib << ",\"schema\":"
                 "\"kilix.voicegen.frontend-benchmark/v1\","
                 "\"warm_request_bytes\":1024,\"warmup_iterations\":100,"
                 "\"latency_microseconds\":{\"median\":"
              << median << ",\"p95\":" << p95 << ",\"p99\":" << p99
              << "}}\n";
    if (p95 > kMaximumP95Microseconds ||
        resource_and_working_kib > kMaximumResourceAndWorkingKib) {
        std::cerr << "frontend resource gate failed\n";
        return 1;
    }
    return 0;
}
