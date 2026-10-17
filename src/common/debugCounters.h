#ifndef KYTY_COMMON_DEBUG_COUNTERS_H_
#define KYTY_COMMON_DEBUG_COUNTERS_H_

#include <atomic>
#include <chrono>
#include <cstdint>

namespace Common::DebugCounters {

static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<bool>::is_always_lock_free);

// Collect events only while the panel is visible. Relaxed, lock-free atomics keep updates
// cheap and safe in signal handlers. Rates come from comparing two snapshots.
enum class Counter {
	// GPU work.
	Draws,
	DrawInstances,
	IndirectDraws,
	Dispatches,
	QueueSubmits,
	GpuWaits, // CPU blocked on a GPU timeline semaphore.
	GpuWaitNs,
	GpuFaults, // Guest writes to GPU-tracked memory.
	ShadersCompiled,
	ShaderCompileNs,
	PipelinesCreated,
	PipelineCreateNs,
	// Caches.
	TextureUploadBytes,
	TextureDownloadBytes,
	TextureEvictions,
	BufferUploadBytes,
	BufferDownloadBytes,
	BufferEvictions,
	// Presentation.
	FlipsRequested,
	FlipsRejected, // Guest flip queue was full.
	LateVblanks,   // The present thread fell behind the vblank rate.
	SwapchainRecreations,
	DroppedPresents,
	// Emulator.
	EmulatedInstructions, // Host CPU lacked an instruction the guest used.
	UnresolvedImportCalls,
	GuestFileReadBytes,
	GuestFileWriteBytes,
	AudioUnderruns,
	// CPU-PROFILE-001A: opt-in (performance overlay visible only).
	// Duration counters accumulate elapsed wall time across threads, NOT CPU utilization.
	CpuAddressWaitCalls,
	CpuAddressImmediateMismatch,
	CpuAddressRegistered,
	CpuAddressRegisteredNs,
	CpuSemaphoreWaitCalls,
	CpuSemaphoreBlocked,
	CpuSemaphoreBlockedNs,
	CpuSemaphoreDrainCalls,
	CpuSemaphoreDrainNs,
	CpuEventFlagParks,
	CpuEventFlagParkNs,
	CpuEventFlagDrainCalls,
	CpuEventFlagDrainNs,
	// CPU-PROFILE-001B: actual wait attempts and guest API family attribution.
	CpuAddressParkCalls,
	CpuAddressParkNs,
	CpuGuestLegacyAddressWaits,
	CpuGuestTypedAddressWaits,
	CpuGuestUmtxAddressWaits,
	CpuGuestKernelAddressWakes,
	CpuGuestUmtxAddressWakes,
	Count
};

enum class Peak { ShaderCompileNs, PipelineCreateNs, Count };

namespace Detail {
inline std::atomic<bool>     g_enabled {false};
inline std::atomic<uint64_t> g_counters[static_cast<int>(Counter::Count)] {};
inline std::atomic<uint64_t> g_peaks[static_cast<int>(Peak::Count)] {};
} // namespace Detail

[[nodiscard]] inline bool Enabled() noexcept {
	return Detail::g_enabled.load(std::memory_order_relaxed);
}

inline void SetEnabled(bool enabled) noexcept {
	Detail::g_enabled.store(enabled, std::memory_order_relaxed);
}

inline void Add(Counter counter, uint64_t value = 1) noexcept {
	if (!Enabled()) {
		return;
	}
	Detail::g_counters[static_cast<int>(counter)].fetch_add(value, std::memory_order_relaxed);
}

// Source attribution is deliberately by HLE API family, never by guest PC.
// Each guest wrapper records exactly one entry before invoking the core primitive.
enum class GuestSyncEntry {
	LegacyAddressWait,
	TypedAddressWait,
	UmtxAddressWait,
	KernelAddressWake,
	UmtxAddressWake,
};

inline void RecordGuestSyncEntry(GuestSyncEntry entry) noexcept {
	Counter counter;
	switch (entry) {
		case GuestSyncEntry::LegacyAddressWait:
			counter = Counter::CpuGuestLegacyAddressWaits;
			break;
		case GuestSyncEntry::TypedAddressWait:
			counter = Counter::CpuGuestTypedAddressWaits;
			break;
		case GuestSyncEntry::UmtxAddressWait:
			counter = Counter::CpuGuestUmtxAddressWaits;
			break;
		case GuestSyncEntry::KernelAddressWake:
			counter = Counter::CpuGuestKernelAddressWakes;
			break;
		case GuestSyncEntry::UmtxAddressWake:
			counter = Counter::CpuGuestUmtxAddressWakes;
			break;
		default: return;
	}
	Add(counter);
}

class ScopedTimer final {
public:
	ScopedTimer(Counter count, Counter duration, Peak peak = Peak::Count)
	    : m_count(count), m_duration(duration), m_peak(peak),
	      m_start(Enabled() ? std::chrono::steady_clock::now()
	                        : std::chrono::steady_clock::time_point {}) {}
	~ScopedTimer() {
		if (m_start == std::chrono::steady_clock::time_point {} || !Enabled()) {
			return;
		}
		const auto elapsed =
		    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		                              std::chrono::steady_clock::now() - m_start)
		                              .count());
		Add(m_count);
		Add(m_duration, elapsed);
		if (m_peak != Peak::Count) {
			auto& peak     = Detail::g_peaks[static_cast<int>(m_peak)];
			auto  previous = peak.load(std::memory_order_relaxed);
			while (previous < elapsed &&
			       !peak.compare_exchange_weak(previous, elapsed, std::memory_order_relaxed)) {
			}
		}
	}
	ScopedTimer(const ScopedTimer&)            = delete;
	ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
	Counter                               m_count;
	Counter                               m_duration;
	Peak                                  m_peak;
	std::chrono::steady_clock::time_point m_start;
};

[[nodiscard]] inline uint64_t Get(Counter counter) noexcept {
	return Detail::g_counters[static_cast<int>(counter)].load(std::memory_order_relaxed);
}

[[nodiscard]] inline uint64_t Get(Peak peak) noexcept {
	return Detail::g_peaks[static_cast<int>(peak)].load(std::memory_order_relaxed);
}

} // namespace Common::DebugCounters

#endif /* KYTY_COMMON_DEBUG_COUNTERS_H_ */
