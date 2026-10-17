// CPU-PROFILE-001B: exercise the same guest HLE classifier used by libKernel.
#include "common/debugCounters.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace Profile = Common::DebugCounters;

namespace {

void Check(bool value, const char* description) {
    if (!value) {
        std::fprintf(stderr, "CpuSyncEntryCountersTests failed: %s\n", description);
        std::abort();
    }
}

struct Case {
    Profile::GuestSyncEntry entry;
    Profile::Counter counter;
};

constexpr std::array<Case, 5> kCases {{
    {Profile::GuestSyncEntry::LegacyAddressWait,
     Profile::Counter::CpuGuestLegacyAddressWaits},
    {Profile::GuestSyncEntry::TypedAddressWait,
     Profile::Counter::CpuGuestTypedAddressWaits},
    {Profile::GuestSyncEntry::UmtxAddressWait,
     Profile::Counter::CpuGuestUmtxAddressWaits},
    {Profile::GuestSyncEntry::KernelAddressWake,
     Profile::Counter::CpuGuestKernelAddressWakes},
    {Profile::GuestSyncEntry::UmtxAddressWake,
     Profile::Counter::CpuGuestUmtxAddressWakes},
}};

void TestDisabledAndEnabled() {
    Profile::SetEnabled(false);
    for (const auto& entry : kCases) {
        const auto before = Profile::Get(entry.counter);
        Profile::RecordGuestSyncEntry(entry.entry);
        Check(Profile::Get(entry.counter) == before,
              "disabled profiler cannot count guest API entry");
    }

    Profile::SetEnabled(true);
    for (const auto& entry : kCases) {
        const auto before = Profile::Get(entry.counter);
        Profile::RecordGuestSyncEntry(entry.entry);
        Check(Profile::Get(entry.counter) == before + 1,
              "guest API classifier increments its own counter");
    }
    Profile::SetEnabled(false);
}

void TestConcurrentExactCounts() {
    constexpr int kWorkers = 4;
    constexpr int kCallsPerWorker = 1000;
    std::array<uint64_t, kCases.size()> before {};
    for (size_t i = 0; i < kCases.size(); ++i) {
        before[i] = Profile::Get(kCases[i].counter);
    }

    Profile::SetEnabled(true);
    std::vector<std::thread> workers;
    for (int w = 0; w < kWorkers; ++w) {
        workers.emplace_back([] {
            for (int n = 0; n < kCallsPerWorker; ++n) {
                for (const auto& entry : kCases) {
                    Profile::RecordGuestSyncEntry(entry.entry);
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    Profile::SetEnabled(false);
    for (size_t i = 0; i < kCases.size(); ++i) {
        Check(Profile::Get(kCases[i].counter) ==
                  before[i] + kWorkers * kCallsPerWorker,
              "concurrent HLE classification must preserve exact event counts");
    }
}

void TestInvalidClassifierValue() {
    Profile::SetEnabled(true);
    std::array<uint64_t, kCases.size()> before {};
    for (size_t i = 0; i < kCases.size(); ++i) {
        before[i] = Profile::Get(kCases[i].counter);
    }
    Profile::RecordGuestSyncEntry(static_cast<Profile::GuestSyncEntry>(255));
    for (size_t i = 0; i < kCases.size(); ++i) {
        Check(Profile::Get(kCases[i].counter) == before[i],
              "unsupported guest source must not change any counter");
    }
    Profile::SetEnabled(false);
}

} // namespace

int main() {
    TestDisabledAndEnabled();
    TestConcurrentExactCounts();
    TestInvalidClassifierValue();
    std::puts("CpuSyncEntryCountersTests: all passed");
    return 0;
}
