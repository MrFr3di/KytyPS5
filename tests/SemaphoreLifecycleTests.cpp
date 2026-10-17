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

// A single cancel/reset must not leave behind a reserved token. A freshly
// signaled token is consumed exactly once after canceled waiters have exited.
void TestCancelCountResetAndReuse() {
    auto sem = NewSema(0, 4);
    int result = -1;
    std::thread waiter([&] {
        result = Sem::KernelWaitSema(sem, 1, nullptr);
    });
    Registered(sem, 1);
    int canceled = -1;
    Check(Sem::KernelCancelSema(sem, 2, &canceled) == OK && canceled == 1,
          "cancel resets count and reports one pending waiter");
    waiter.join();
    Check(result == Libs::LibKernel::KERNEL_ERROR_ECANCELED,
          "previous waiter must not consume reset count");
    Check(Sem::KernelPollSema(sem, 2) == OK,
          "reset count available to a new consumer");
    Check(Sem::KernelPollSema(sem, 1) == Libs::LibKernel::KERNEL_ERROR_EBUSY,
          "reset tokens cannot be consumed twice");
    Check(Sem::KernelDeleteSema(sem) == OK, "delete semaphore after reuse");
}

// Concurrent Signal and Cancel linearize under the same mutex. Whichever wins,
// each waiter must get exactly one terminal result and Cancel's waiter count
// must match the number it actually canceled. Deletion begins only AFTER the
// producer threads join, never with an untracked call on a freed handle.
void TestSignalCancelRace() {
    constexpr int rounds = 80;
    for (int round = 0; round < rounds; ++round) {
        auto sem = NewSema(0, 2);
        int results[2] = {-1, -1};
        std::thread waiter0([&] { results[0] = Sem::KernelWaitSema(sem, 1, nullptr); });
        Registered(sem, 1);
        std::thread waiter1([&] { results[1] = Sem::KernelWaitSema(sem, 1, nullptr); });
        Registered(sem, 2);
        std::atomic<bool> go{false};
        int cancel_count = -1;
        int signal_result = -1;
        std::thread signaler([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            signal_result = Sem::KernelSignalSema(sem, 1);
        });
        std::thread canceler([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            (void)Sem::KernelCancelSema(sem, 0, &cancel_count);
        });
        go.store(true, std::memory_order_release);
        signaler.join();
        canceler.join();
        waiter0.join();
        waiter1.join();
        const int succeeded = (results[0] == OK ? 1 : 0) + (results[1] == OK ? 1 : 0);
        const int canceled = (results[0] == Libs::LibKernel::KERNEL_ERROR_ECANCELED ? 1 : 0) +
                             (results[1] == Libs::LibKernel::KERNEL_ERROR_ECANCELED ? 1 : 0);
        Check(signal_result == OK, "competing signal succeeds");
        Check(succeeded <= 1 && succeeded + canceled == 2,
              "signal/cancel race yields exactly two terminal results");
        Check(cancel_count == canceled, "cancel count matches canceled results");
        Check(Sem::KernelSemaWaitingCountForTests(sem) == 0,
              "competing signal and cancel fully drain waiter vector");
        Check(Sem::KernelDeleteSema(sem) == OK, "delete after signal/cancel actors join");
    }
}

// A partially fulfilled wait set may be deleted immediately: waiters already
// granted tokens must keep OK; not-yet-selected waiters must receive Deleted.
void TestPartialSignalThenDelete() {
    constexpr int rounds = 40;
    constexpr int waiters = 4;
    for (int round = 0; round < rounds; ++round) {
        auto sem = NewSema(0, 4);
        int results[waiters] = {-1, -1, -1, -1};
        std::vector<std::thread> threads;
        for (int i = 0; i < waiters; ++i) {
            threads.emplace_back([&, i] {
                results[i] = Sem::KernelWaitSema(sem, 1, nullptr);
            });
        }
        Registered(sem, waiters);
        Check(Sem::KernelSignalSema(sem, 2) == OK, "grant two tokens before deletion");
        Check(Sem::KernelDeleteSema(sem) == OK,
              "delete waits until partially released guest waiters drain");
        for (auto& thread : threads) thread.join();
        int completed = 0;
        int deleted = 0;
        for (int result : results) {
            completed += result == OK ? 1 : 0;
            deleted += result == Libs::LibKernel::KERNEL_ERROR_EACCES ? 1 : 0;
        }
        Check(completed == 2 && deleted == 2,
              "pre-granted waits remain successful and other waits are deleted");
    }
}

// Race an expiring timeout with cancellation, but only cancel while the
// semaphore object remains valid. Timeout removes its waiter under m_mutex;
// Cancel counts only pending entries. Either ordering is permitted.
void TestTimeoutCancelRace() {
    constexpr int rounds = 18;
    for (int round = 0; round < rounds; ++round) {
        auto sem = NewSema();
        int result = -1;
        Libs::LibKernel::KernelUseconds remaining = 100000;
        std::thread waiter([&] {
            result = Sem::KernelWaitSema(sem, 1, &remaining);
        });
        Registered(sem, 1);
        // Exercise pre-deadline and post-deadline cancellation.
        if (round % 3 == 1) std::this_thread::sleep_for(std::chrono::milliseconds(30));
        if (round % 3 == 2) std::this_thread::sleep_for(std::chrono::milliseconds(140));
        int canceled = -1;
        Check(Sem::KernelCancelSema(sem, 0, &canceled) == OK,
              "timeout/cancel race call");
        waiter.join();
        Check((canceled == 1 && result == Libs::LibKernel::KERNEL_ERROR_ECANCELED) ||
              (canceled == 0 && result == Libs::LibKernel::KERNEL_ERROR_ETIMEDOUT),
              "cancel count and timeout/cancel result must be consistent");
        Check(Sem::KernelSemaWaitingCountForTests(sem) == 0,
              "timeout/cancel transition drains waiter state");
        Check(remaining <= 100000, "wait never increases deadline budget");
        Check(Sem::KernelDeleteSema(sem) == OK,
              "timeout/cancel race deletion with no outstanding access");
    }
}

// Priority order alone is insufficient: an earlier higher-priority waiter
// needing 3 tokens must not block the lower-priority waiter needing 1 when
// only 2 tokens become available.
void TestWeightedPriorityBypass() {
    auto sem = NewSema(0, 4, 2);
    int high_result = -1;
    int low_result = -1;
    std::atomic<int> low_done{0};
    std::thread high([&] {
        g_priority = 100;
        high_result = Sem::KernelWaitSema(sem, 3, nullptr);
    });
    Registered(sem, 1);
    std::thread low([&] {
        g_priority = 900;
        low_result = Sem::KernelWaitSema(sem, 1, nullptr);
        low_done.store(1, std::memory_order_release);
    });
    Registered(sem, 2);
    Check(Sem::KernelSignalSema(sem, 2) == OK, "signal two units");
    Eventually([&] { return low_done.load(std::memory_order_acquire) == 1; },
               "available token bypasses unsatisfiable high priority waiter");
    Check(low_result == OK, "lower priority waiter consumes one unit");
    Check(Sem::KernelSignalSema(sem, 2) == OK,
          "add two more units to satisfy high priority waiter");
    high.join();
    low.join();
    Check(high_result == OK, "weighted high-priority waiter eventually completes");
    Check(Sem::KernelPollSema(sem, 1) == Libs::LibKernel::KERNEL_ERROR_EBUSY,
          "all weighted signal tokens accounted for");
    Check(Sem::KernelDeleteSema(sem) == OK, "delete after weighted priority test");
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
    TestCancelCountResetAndReuse();
    TestSignalCancelRace();
    TestPartialSignalThenDelete();
    TestTimeoutCancelRace();
    TestWeightedPriorityBypass();
    TestGuestSignalPolling();
    std::puts("SemaphoreLifecycleTests: all passed");
    return 0;
}
