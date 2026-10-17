// CPU-SYNC-001: measure the actual exported Wait32/Wait64 paths, CPU only.
// Runs in separate processes linked against the baseline or candidate source.
// Measurements are synthetic and do not imply an improvement in game FPS.
#include "kernel/syncOnAddress.h"
#include "common/debugCounters.h"
#include "libs/errno.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr int kRounds = 9;
constexpr uint32_t kMismatchIterations = 100000;
constexpr uint32_t kTimeoutIterations = 3000;
uint32_t g_word32 = 1;
uint64_t g_word64 = 1;
std::atomic<uint32_t> g_poll_calls{0};

void Require(bool valid, const char* message) {
    if (!valid) {
        std::fprintf(stderr, "CPU-SYNC-001: %s\n", message);
        std::abort();
    }
}
void Poll() { g_poll_calls.fetch_add(1, std::memory_order_relaxed); }

template<class Call>
void Measure(const char* label, uint32_t iterations, int wanted, Call call) {
    for (int warmup = 0; warmup < 1024; ++warmup) {
        Require(call() == wanted, "warmup return code mismatch");
    }
    std::array<double, kRounds> ns{};
    for (int round = 0; round < kRounds; ++round) {
        const auto start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < iterations; ++i) {
            Require(call() == wanted, "benchmark return code mismatch");
        }
        const auto stop = std::chrono::steady_clock::now();
        ns[round] = std::chrono::duration<double, std::nano>(stop - start).count()
                  / static_cast<double>(iterations);
    }
    std::sort(ns.begin(), ns.end());
    std::printf("%s,%u,%d,%.3f,%.3f,%.3f\n", label, iterations,
                kRounds, ns.front(), ns[kRounds / 2], ns.back());
}
} // namespace

int main(int argc, char** argv) {
    // Default preserves the frozen SyncOnAddress comparison's disabled profile.
    // The explicit modes allow same-source candidate OFF/ON/ON/OFF calibration.
    bool enable_profile = false;
    if (argc == 2 && std::strcmp(argv[1], "--profile-on") == 0) {
        enable_profile = true;
    } else if (argc == 2 && std::strcmp(argv[1], "--profile-off") == 0) {
        enable_profile = false;
    } else if (argc != 1) {
        std::fprintf(stderr, "Usage: sync_on_address_cpu_bench [--profile-on|--profile-off]\n");
        return 2;
    }
    Common::DebugCounters::SetEnabled(false);
    using Libs::LibKernel::SyncOnAddress::Wait32;
    using Libs::LibKernel::SyncOnAddress::Wait64;
    constexpr uint32_t kChecks = 1024;
    for (uint32_t i = 0; i < kChecks; ++i) {
        Require(Wait32(&g_word32, 0, nullptr, Poll) == OK, "mismatch32");
        Require(Wait64(&g_word64, 0, nullptr, Poll) == OK, "mismatch64");
    }
    Require(g_poll_calls.load(std::memory_order_relaxed) == kChecks * 2,
            "mismatched waits must poll guest signals exactly once");

    Common::DebugCounters::SetEnabled(enable_profile);
    std::puts("case,iterations,rounds,min_ns,median_ns,max_ns");
    Measure("mismatch32", kMismatchIterations, OK, [] {
        return Wait32(&g_word32, 0, nullptr);
    });
    Measure("mismatch64", kMismatchIterations, OK, [] {
        return Wait64(&g_word64, 0, nullptr);
    });
    Measure("timeout32_zero", kTimeoutIterations,
            Libs::LibKernel::KERNEL_ERROR_ETIMEDOUT, [] {
                uint32_t zero = 0;
                return Wait32(&g_word32, 1, &zero);
            });
    Measure("timeout64_zero", kTimeoutIterations,
            Libs::LibKernel::KERNEL_ERROR_ETIMEDOUT, [] {
                uint32_t zero = 0;
                return Wait64(&g_word64, 1, &zero);
            });
    Common::DebugCounters::SetEnabled(false);
    return 0;
}
