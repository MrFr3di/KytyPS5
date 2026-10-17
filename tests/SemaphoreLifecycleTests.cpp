// CPU-SYNC-002B: isolated native kernel semaphore lifecycle and ordering tests.
// Test-only kernel shims provide guest services; the semaphore implementation
// is the production translation unit with its regular wait and wake code.
#include "kernel/semaphore.h"
#include "libs/errno.h"
#include "common/threads.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace Sem = Libs::LibKernel::Semaphore;
namespace Libs::LibKernel::Semaphore {
int KernelSemaWaitingCountForTests(KernelSema);
}

namespace {
thread_local int g_priority = 700;
std::atomic<int> g_guest_signal_poll_count{0};
void Check(bool yes, const char* description) {
    if (!yes) {
        std::fprintf(stderr, "SemaphoreLifecycleTests failed: %s\n", description);
        std::abort();
    }
}
template <typename F>
void Eventually(F condition, const char* description) {
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!condition() && std::chrono::steady_clock::now() < limit) {
        std::this_thread::yield();
    }
    Check(condition(), description);
}
Sem::KernelSema NewSema(int init = 0, int max = 8, int attr = 1) {
    Sem::KernelSema sem = nullptr;
    Check(Sem::KernelCreateSema(&sem, "cpu-sema-drain", attr, init, max, nullptr) == OK,
          "create native semaphore");
    Check(sem != nullptr, "semaphore handle");
    return sem;
}
void Registered(Sem::KernelSema sem, int n) {
    Eventually([&] { return Sem::KernelSemaWaitingCountForTests(sem) == n; },
               "waiters registered before lifecycle operation");
}
} // namespace

// Link shims for this standalone test only (never compiled into the emulator).
namespace Libs::LibKernel {
int PthreadGetCurrentPriorityForKernel() { return g_priority; }
void KernelDispatchPendingSignalForCurrentThread() {
    g_guest_signal_poll_count.fetch_add(1, std::memory_order_relaxed);
}
int KYTY_SYSV_ABI KernelClockGettime(KernelClockid, KernelTimespec* now) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (now != nullptr) {
        now->tv_sec = ns / 1000000000;
        now->tv_nsec = ns % 1000000000;
    }
    return OK;
}
} // namespace Libs::LibKernel
namespace Libs::Posix {
int* GetErrorAddr() {
    thread_local int value = 0;
    return &value;
}
} // namespace Libs::Posix

namespace {
constexpr int kRounds = 20;
constexpr int kWaiters = 4;

void TestImmediateAndTimeout() {
    auto sem = NewSema(2, 3);
    Check(Sem::KernelPollSema(sem, 1) == OK, "first immediate decrement");
    Check(Sem::KernelPollSema(sem, 1) == OK, "second immediate decrement");
    Check(Sem::KernelPollSema(sem, 1) == Libs::LibKernel::KERNEL_ERROR_EBUSY,
          "poll empty returns busy");
    Libs::LibKernel::KernelUseconds zero = 0;
    Check(Sem::KernelWaitSema(sem, 1, &zero) ==
          Libs::LibKernel::KERNEL_ERROR_ETIMEDOUT, "zero timed wait expires");
    Check(Sem::KernelSignalSema(sem, 3) == OK, "signal maximum");
    Check(Sem::KernelSignalSema(sem, 1) ==
          Libs::LibKernel::KERNEL_ERROR_EINVAL, "max count is enforced");
    Check(Sem::KernelDeleteSema(sem) == OK, "delete quiescent semaphore");
}

void TestCancelAndReuse() {
    for (int round = 0; round < kRounds; ++round) {
        auto sem = NewSema();
        int results[kWaiters]{};
        std::vector<std::thread> threads;
        for (int i = 0; i < kWaiters; ++i) {
            threads.emplace_back([&, i] {
                Libs::LibKernel::KernelUseconds timeout = 1500000;
                results[i] = Sem::KernelWaitSema(sem, 1, &timeout);
            });
        }
        Registered(sem, kWaiters);
        int canceled = -1;
        Check(Sem::KernelCancelSema(sem, 0, &canceled) == OK,
              "cancel registered native semaphore waiters");
        Check(canceled == kWaiters, "cancel count equals registered waiters");
        // Delete immediately, before joining the canceled worker threads:
        // destructor must preserve ready=Canceled and drain their stack entries.
        Check(Sem::KernelDeleteSema(sem) == OK, "delete following cancellation");
        for (auto& thread : threads) thread.join();
        for (int result : results) {
            Check(result == Libs::LibKernel::KERNEL_ERROR_ECANCELED,
                  "cancel result preserved across immediate deletion");
        }
    }
}

void TestDeleteRegisteredWaiters() {
    for (int round = 0; round < kRounds; ++round) {
        auto sem = NewSema();
        int results[kWaiters]{};
        std::vector<std::thread> threads;
        for (int i = 0; i < kWaiters; ++i) {
            threads.emplace_back([&, i] {
                Libs::LibKernel::KernelUseconds timeout = 1500000;
                results[i] = Sem::KernelWaitSema(sem, 1, &timeout);
            });
        }
        Registered(sem, kWaiters);
        Check(Sem::KernelDeleteSema(sem) == OK, "delete drains all waiters");
        for (auto& thread : threads) thread.join();
        for (int result : results) {
            Check(result == Libs::LibKernel::KERNEL_ERROR_EACCES,
                  "blocked wait returns deleted status");
        }
    }
}

void TestSignalOrdering(int attr, int priority0, int priority1, int winner) {
    auto sem = NewSema(0, 2, attr);
    int results[2] = {-1, -1};
    std::atomic<int> done{0};
    std::atomic<int> first{-1};
    std::vector<std::thread> threads;
    threads.emplace_back([&] {
        g_priority = priority0;
        results[0] = Sem::KernelWaitSema(sem, 1, nullptr);
        int unset = -1;
        (void)first.compare_exchange_strong(unset, 0);
        done.fetch_add(1);
    });
    Registered(sem, 1);
    threads.emplace_back([&] {
        g_priority = priority1;
        results[1] = Sem::KernelWaitSema(sem, 1, nullptr);
        int unset = -1;
        (void)first.compare_exchange_strong(unset, 1);
        done.fetch_add(1);
    });
    Registered(sem, 2);
    Check(Sem::KernelSignalSema(sem, 1) == OK, "signal one waiter");
    Eventually([&] { return done.load() == 1; }, "one waiter released");
    Check(first.load() == winner, "FIFO or priority ordering preserved");
    int canceled = -1;
    Check(Sem::KernelCancelSema(sem, 0, &canceled) == OK,
          "cancel remaining waiter");
    Check(canceled == 1, "only one waiter remains");
    for (auto& thread : threads) thread.join();
    Check(results[winner] == OK, "selected waiter succeeded");
    Check(results[1 - winner] == Libs::LibKernel::KERNEL_ERROR_ECANCELED,
          "other waiter canceled");
    Check(Sem::KernelDeleteSema(sem) == OK, "delete ordered semaphore");
}

void TestGuestSignalPolling() {
    g_guest_signal_poll_count.store(0);
    auto sem = NewSema();
    int result = -1;
    std::thread waiter([&] {
        result = Sem::KernelWaitSema(sem, 1, nullptr);
    });
    Registered(sem, 1);
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (g_guest_signal_poll_count.load() < 2 &&
           std::chrono::steady_clock::now() < limit) {
        std::this_thread::yield();
    }
    Check(Sem::KernelSignalSema(sem, 1) == OK, "signal after guest safe points");
    waiter.join();
    Check(result == OK, "guest wait completes normally");
    Check(g_guest_signal_poll_count.load() >= 2,
          "native semaphore wait retains guest signal polling");
    Check(Sem::KernelDeleteSema(sem) == OK, "delete after signal polling");
}
} // namespace

int main() {
    TestImmediateAndTimeout();
    TestCancelAndReuse();
    TestDeleteRegisteredWaiters();
    TestSignalOrdering(1, 900, 100, 0); // FIFO beats higher priority.
    TestSignalOrdering(2, 900, 100, 1); // Priority order selects thread 1.
    TestGuestSignalPolling();
    std::puts("SemaphoreLifecycleTests: all passed");
    return 0;
}
