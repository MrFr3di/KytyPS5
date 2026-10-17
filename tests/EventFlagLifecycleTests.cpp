// CPU-SYNC-002A: deterministic EventFlag cancel/delete waiter-drain oracle.
// Use the production EventFlag code with an observer compiled ONLY in this test.
// The observer never modifies the wait-state or participates in the wake protocol.
#include "kernel/eventFlag.h"
#include "libs/errno.h"

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
} // namespace

int main() {
    TestZeroTimeoutAndState();
    TestCancelRegisteredWaiters();
    TestDeleteRegisteredWaiters();
    std::puts("EventFlagLifecycleTests: all passed");
    return 0;
}
