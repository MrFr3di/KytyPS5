// CPU-SYNC-001: synthetic microbenchmark for host-side address waits.
// No gameplay/FPS claims. Each result is local to the GitHub-hosted runner.
#include "kernel/syncOnAddress.h"
#include "libs/errno.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {
using Libs::LibKernel::SyncOnAddress::Wait32;
using Libs::LibKernel::SyncOnAddress::Wait64;

constexpr int      Rounds = 9;
constexpr uint32_t MismatchIterations = 250000;
constexpr uint32_t MatchIterations = 12000;

uint32_t word32 = 1;
uint64_t word64 = 1;
std::atomic<uint32_t> signal_polls {0};

void Require(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "CPU-SYNC-001 FAILED: %s\n", message);
        std::abort();
    }
}

void PollSignal() {
    signal_polls.fetch_add(1, std::memory_order_relaxed);
}

template <typename F>
void RunCase(const char* name, uint32_t iterations, int expected_result, F call) {
    for (uint32_t n = 0; n < 2048; ++n) {
        Require(call() == expected_result, "warmup result");
    }

    std::array<double, Rounds> latency {};
    for (int round = 0; round < Rounds; ++round) {
        const auto begin = std::chrono::steady_clock::now();
        for (uint32_t n = 0; n < iterations; ++n) {
            Require(call() == expected_result, "measured result");
        }
        const auto finish = std::chrono::steady_clock::now();
        const auto elapsed_ns =
            std::chrono::duration<double, std::nano>(finish - begin).count();
        latency[round] = elapsed_ns / static_cast<double>(iterations);
    }
    std::sort(latency.begin(), latency.end());
    std::printf("%s,%u,%d,%.3f,%.3f,%.3f,%d\n", name, iterations, Rounds,
                latency[Rounds / 2], latency.front(), latency.back(), expected_result);
}
} // namespace

int main() {
    // Verify the mismatch fast path remains a guest signal-delivery safe point.
    constexpr uint32_t Checks = 4096;
    for (uint32_t i = 0; i < Checks; ++i) {
        Require(Wait32(&word32, 0u, nullptr, PollSignal) == OK,
                "mismatch32 should return without parking");
        Require(Wait64(&word64, 0ull, nullptr, PollSignal) == OK,
                "mismatch64 should return without parking");
    }
    Require(signal_polls.load(std::memory_order_relaxed) == Checks * 2,
            "each mismatched wait must poll guest signals once");

    std::printf("case,iterations,rounds,median_ns_per_op,min_ns_per_op,max_ns_per_op,result\n");
    RunCase("mismatch32", MismatchIterations, OK,
            [] { return Wait32(&word32, 0u, nullptr); });
    RunCase("mismatch64", MismatchIterations, OK,
            [] { return Wait64(&word64, 0ull, nullptr); });
    RunCase("matching32_zero_timeout", MatchIterations,
            Libs::LibKernel::KERNEL_ERROR_ETIMEDOUT,
            [] { uint32_t zero = 0; return Wait32(&word32, 1u, &zero); });
    RunCase("matching64_zero_timeout", MatchIterations,
            Libs::LibKernel::KERNEL_ERROR_ETIMEDOUT,
            [] { uint32_t zero = 0; return Wait64(&word64, 1ull, &zero); });
    std::puts("CPU_SYNC_001_PASS");
    return 0;
}
