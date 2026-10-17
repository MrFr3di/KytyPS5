// CPU-SYNC-002A: deterministic EventFlag cancel/delete waiter-drain oracle.
// Use the production EventFlag code with an observer compiled ONLY in this test.
// The observer never modifies the wait-state or participates in the wake protocol.
#include "kernel/eventFlag.h"
#include "libs/errno.h"
#include "common/threads.h"
#include "common/debugCounters.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace EventFlag = Libs::LibKernel::EventFlag;
namespace Libs::LibKernel::EventFlag {
int KernelEventFlagWaiterCountForTests(KernelEventFlag ef);
}

namespace {
using Libs::LibKernel::KERNEL_ERROR_EACCES;
using Libs::LibKernel::KERNEL_ERROR_EBUSY;
using Libs::LibKernel::KERNEL_ERROR_ECANCELED;
using Libs::LibKernel::KERNEL_ERROR_ETIMEDOUT;

void Check(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "EventFlagLifecycleTests failed: %s\n", what);
        std::abort();
    }
}

std::atomic<int> g_guest_signal_callbacks{0};
void TrackGuestSignalPoll() {
    g_guest_signal_callbacks.fetch_add(1, std::memory_order_relaxed);
}

constexpr int kWaiters = 4;
constexpr int kIterations = 20;

EventFlag::KernelEventFlag NewFlag() {
    EventFlag::KernelEventFlag ef = nullptr;
    Check(EventFlag::KernelCreateEventFlag(&ef, "cpu-flag-drain", 0x20u, 0, nullptr) == OK,
          "create multi-waiter flag");
    Check(ef != nullptr, "non-null handle");
    return ef;
}

void WaitUntilRegistered(EventFlag::KernelEventFlag ef, int expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (EventFlag::KernelEventFlagWaiterCountForTests(ef) != expected &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    Check(EventFlag::KernelEventFlagWaiterCountForTests(ef) == expected,
          "all worker threads registered with the flag before cancel/delete");
}

void TestZeroTimeoutAndState() {
    auto ef = NewFlag();
    Libs::LibKernel::KernelUseconds timeout = 0;
    uint64_t result = 0;
    Check(EventFlag::KernelWaitEventFlag(ef, 1, 0x01, &result, &timeout) ==
              KERNEL_ERROR_ETIMEDOUT, "zero-timeout unmatched wait");
    Check(EventFlag::KernelPollEventFlag(ef, 1, 0x01, &result) == KERNEL_ERROR_EBUSY,
          "poll returns busy without requested bits");
    Check(EventFlag::KernelSetEventFlag(ef, 1) == OK, "set bit");
    Check(EventFlag::KernelPollEventFlag(ef, 1, 0x01, &result) == OK && (result & 1) != 0,
          "set bit releases poll");
    Check(EventFlag::KernelClearEventFlag(ef, 0) == OK, "clear bits");
    Check(EventFlag::KernelPollEventFlag(ef, 1, 0x01, &result) == KERNEL_ERROR_EBUSY,
          "clear bit restores busy poll");
    Check(EventFlag::KernelDeleteEventFlag(ef) == OK, "delete unused flag");
}

void TestGuestSignalSafePoint() {
    // Guest-side waits must retain their normal signal polling behavior.
    // Only the host-side cancellation/deletion drain bypasses the callback.
    g_guest_signal_callbacks.store(0, std::memory_order_relaxed);
    Common::CondVar::SetWaitPollCallback(TrackGuestSignalPoll);
    auto ef = NewFlag();
    int result = KERNEL_ERROR_ETIMEDOUT;
    std::thread waiter([&] {
        result = EventFlag::KernelWaitEventFlag(ef, 1, 0x01, nullptr, nullptr);
    });
    WaitUntilRegistered(ef, 1);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (g_guest_signal_callbacks.load(std::memory_order_relaxed) < 2 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    Check(EventFlag::KernelSetEventFlag(ef, 1) == OK,
          "set releases guest waiter with signal callback installed");
    waiter.join();
    Common::CondVar::SetWaitPollCallback(nullptr);
    Check(result == OK, "guest waiter still wakes normally");
    Check(g_guest_signal_callbacks.load(std::memory_order_relaxed) >= 2,
          "guest wait continues polling guest signals");
    Check(EventFlag::KernelDeleteEventFlag(ef) == OK,
          "delete flag after guest signal safe point");
}

void TestCancelRegisteredWaiters() {
    for (int iteration = 0; iteration < kIterations; ++iteration) {
        auto ef = NewFlag();
        int results[kWaiters]{};
        uint64_t result_bits[kWaiters]{};
        std::vector<std::thread> waiters;
        for (int i = 0; i < kWaiters; ++i) {
            waiters.emplace_back([&, i] {
                Libs::LibKernel::KernelUseconds timeout = 1500000;
                results[i] = EventFlag::KernelWaitEventFlag(
                    ef, 1, 0x01, &result_bits[i], &timeout);
            });
        }
        WaitUntilRegistered(ef, kWaiters);
        int canceled = -1;
        Check(EventFlag::KernelCancelEventFlag(ef, 0xa4, &canceled) == OK,
              "cancel registered waiters");
        Check(canceled == kWaiters, "cancel counts all registered waiters");
        for (auto& waiter : waiters) {
            waiter.join();
        }
        Check(EventFlag::KernelEventFlagWaiterCountForTests(ef) == 0,
              "cancel returns only after waiter drain");
        for (int i = 0; i < kWaiters; ++i) {
            Check(results[i] == KERNEL_ERROR_ECANCELED, "canceled waiter return code");
            Check(result_bits[i] == 0xa4, "canceled waiter observes cancel bits");
        }
        Check(EventFlag::KernelSetEventFlag(ef, 1) == OK,
              "flag usable again once canceled waiters have drained");
        Check(EventFlag::KernelDeleteEventFlag(ef) == OK, "delete after cancel");
    }
}

void TestDeleteRegisteredWaiters() {
    for (int iteration = 0; iteration < kIterations; ++iteration) {
        auto ef = NewFlag();
        int results[kWaiters]{};
        std::vector<std::thread> waiters;
        for (int i = 0; i < kWaiters; ++i) {
            waiters.emplace_back([&, i] {
                Libs::LibKernel::KernelUseconds timeout = 1500000;
                uint64_t result = 0;
                results[i] = EventFlag::KernelWaitEventFlag(ef, 1, 0x01, &result, &timeout);
            });
        }
        WaitUntilRegistered(ef, kWaiters);
        Check(EventFlag::KernelDeleteEventFlag(ef) == OK,
              "delete waits for all already registered waiters to exit");
        // The pointer is invalid after deletion; do not inspect the object.
        for (auto& waiter : waiters) {
            waiter.join();
        }
        for (int result : results) {
            Check(result == KERNEL_ERROR_EACCES, "deleted waiter return code");
        }
    }
}
void TestProfileCountersOptIn() {
    using namespace Common::DebugCounters;
    const auto before = Get(Counter::CpuEventFlagParks);
    auto ef = NewFlag();
    SetEnabled(true);
    int result = -1;
    std::thread waiter([&] {
        result = EventFlag::KernelWaitEventFlag(ef, 1, 0x01, nullptr, nullptr);
    });
    WaitUntilRegistered(ef, 1);
    Check(EventFlag::KernelSetEventFlag(ef, 1) == OK,
          "profiled event flag set releases registered waiter");
    waiter.join();
    Check(result == OK, "profiled event flag wake succeeded");
    Check(Get(Counter::CpuEventFlagParks) > before,
          "event flag profiler counts actual guest park");
    SetEnabled(false);
    const auto frozen = Get(Counter::CpuEventFlagParks);
    Check(EventFlag::KernelDeleteEventFlag(ef) == OK,
          "delete after profiling ends");
    Check(Get(Counter::CpuEventFlagParks) == frozen,
          "disabled event flag profiling remains inactive");
}

} // namespace

int main() {
    TestZeroTimeoutAndState();
    TestGuestSignalSafePoint();
    TestCancelRegisteredWaiters();
    TestDeleteRegisteredWaiters();
    TestProfileCountersOptIn();
    std::puts("EventFlagLifecycleTests: all passed");
    return 0;
}
