#include "graphics/host_gpu/renderer/pipeline/descriptorHeap.h"

#include "common/assert.h"
#include "common/profiler.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"
#if defined(KYTY_DESCRIPTOR_HEAP_TELEMETRY) && KYTY_DESCRIPTOR_HEAP_TELEMETRY
#include <cstdio>
#include <cstdlib>
#include <cstring>
#endif

namespace Libs::Graphics {
namespace {

constexpr uint32_t   DescriptorHeapCount = 1024;
constexpr std::array DescriptorPoolSizes = {
    vk::DescriptorPoolSize {vk::DescriptorType::eStorageBuffer, 8192},
    vk::DescriptorPoolSize {vk::DescriptorType::eSampledImage, 8192},
    vk::DescriptorPoolSize {vk::DescriptorType::eStorageImage, 1024},
    vk::DescriptorPoolSize {vk::DescriptorType::eSampler, 1024},
};

} // namespace

DescriptorHeap::DescriptorHeap(GraphicContext& graphics, MasterSemaphore& master_semaphore)
    : m_graphics(graphics), m_master_semaphore(master_semaphore) {
#if defined(KYTY_DESCRIPTOR_HEAP_TELEMETRY) && KYTY_DESCRIPTOR_HEAP_TELEMETRY
	const char* trace = std::getenv("KYTY_DESCRIPTOR_HEAP_TRACE");
	m_telemetry_enabled = trace != nullptr && std::strcmp(trace, "1") == 0;
#endif
	CreateDescriptorPool();
}

DescriptorHeap::~DescriptorHeap() {
#if defined(KYTY_DESCRIPTOR_HEAP_TELEMETRY) && KYTY_DESCRIPTOR_HEAP_TELEMETRY
	// Snapshot BEFORE teardown: physical pool cleanup would distort production churn.
	if (m_telemetry_enabled) {
		if (auto* file = std::fopen("descriptor-heap-telemetry.csv", "w")) {
			m_telemetry.WriteCsv(file);
			std::fclose(file);
		}
	}
#endif
	m_graphics.device.destroyDescriptorPool(m_current_pool, nullptr);
	for (const auto& [pool, tick]: m_pending_pools) {
		m_master_semaphore.Wait(tick);
		m_graphics.device.destroyDescriptorPool(pool, nullptr);
	}
}

vk::DescriptorSet DescriptorHeap::Commit(vk::DescriptorSetLayout layout
#if defined(KYTY_DESCRIPTOR_HEAP_TELEMETRY) && KYTY_DESCRIPTOR_HEAP_TELEMETRY
                                        , const std::array<uint64_t,4>& types_per_set,
                                        bool types_known
#endif
) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(layout == nullptr);

	auto& batch = m_sets[layout];
#if defined(KYTY_DESCRIPTOR_HEAP_TELEMETRY) && KYTY_DESCRIPTOR_HEAP_TELEMETRY
	// A successfully cached batch owns its original type vector. On refill
	// (size==0), use the just-created pipeline layout's fresh metadata.
	if (m_telemetry_enabled && batch.size == 0) {
		batch.diagnostic_types_per_set = types_per_set;
		batch.diagnostic_types_known = types_known;
	}
#endif
	if (batch.size != 0) {
#if defined(KYTY_DESCRIPTOR_HEAP_TELEMETRY) && KYTY_DESCRIPTOR_HEAP_TELEMETRY
		if (m_telemetry_enabled) {
			m_telemetry.CacheHit();
			m_telemetry.Issued();
			m_telemetry.IssuedTyped(batch.diagnostic_types_per_set,
			                       batch.diagnostic_types_known);
		}
#endif
		return batch.sets[--batch.size];
	}
#if defined(KYTY_DESCRIPTOR_HEAP_TELEMETRY) && KYTY_DESCRIPTOR_HEAP_TELEMETRY
	if (m_telemetry_enabled) m_telemetry.CacheMiss();
#endif
	if (Allocate(layout, batch)) {
#if defined(KYTY_DESCRIPTOR_HEAP_TELEMETRY) && KYTY_DESCRIPTOR_HEAP_TELEMETRY
		if (m_telemetry_enabled) {
			m_telemetry.Issued();
			m_telemetry.IssuedTyped(batch.diagnostic_types_per_set,
			                       batch.diagnostic_types_known);
		}
#endif
		return batch.sets[--batch.size];
	}

	m_pending_pools.emplace_back(m_current_pool, m_master_semaphore.CurrentTick());
#if defined(KYTY_DESCRIPTOR_HEAP_TELEMETRY) && KYTY_DESCRIPTOR_HEAP_TELEMETRY
	if (m_telemetry_enabled) m_telemetry.PoolRetired(m_pending_pools.size());
#endif
	if (const auto& [pool, tick] = m_pending_pools.front(); m_master_semaphore.IsFree(tick)) {
		m_current_pool = pool;
		m_pending_pools.pop_front();
		EXIT_IF(m_graphics.device.resetDescriptorPool(m_current_pool, {}) != vk::Result::eSuccess);
#if defined(KYTY_DESCRIPTOR_HEAP_TELEMETRY) && KYTY_DESCRIPTOR_HEAP_TELEMETRY
		if (m_telemetry_enabled) m_telemetry.PoolReset();
#endif
	} else {
		CreateDescriptorPool();
	}

#if defined(KYTY_DESCRIPTOR_HEAP_TELEMETRY) && KYTY_DESCRIPTOR_HEAP_TELEMETRY
	if (m_telemetry_enabled) {
		uint64_t discarded = 0;
		std::array<uint64_t,4> discarded_types {};
		bool discarded_types_known = true;
		for (const auto& [cached_layout, cached_batch]: m_sets) {
			(void)cached_layout;
			discarded += cached_batch.size;
			if (cached_batch.size != 0 && !cached_batch.diagnostic_types_known)
				discarded_types_known = false;
			if (cached_batch.diagnostic_types_known) {
				for (size_t i=0;i<4;++i)
					discarded_types[i] += uint64_t(cached_batch.size) *
					                      cached_batch.diagnostic_types_per_set[i];
			}
		}
		m_telemetry.DiscardUnused(discarded);
		m_telemetry.DiscardTyped(discarded, discarded_types, discarded_types_known);
	}
#endif
	m_sets.clear();
	auto& fresh_batch = m_sets[layout];
#if defined(KYTY_DESCRIPTOR_HEAP_TELEMETRY) && KYTY_DESCRIPTOR_HEAP_TELEMETRY
	if (m_telemetry_enabled) {
		fresh_batch.diagnostic_types_per_set = types_per_set;
		fresh_batch.diagnostic_types_known = types_known;
	}
#endif
	EXIT_IF(!Allocate(layout, fresh_batch));
#if defined(KYTY_DESCRIPTOR_HEAP_TELEMETRY) && KYTY_DESCRIPTOR_HEAP_TELEMETRY
	if (m_telemetry_enabled) {
		m_telemetry.Issued();
		m_telemetry.IssuedTyped(fresh_batch.diagnostic_types_per_set,
		                       fresh_batch.diagnostic_types_known);
	}
#endif
	return fresh_batch.sets[--fresh_batch.size];
}

bool DescriptorHeap::Allocate(vk::DescriptorSetLayout layout, Batch& batch) {
	std::array<vk::DescriptorSetLayout, DescriptorSetBatch> layouts;
	layouts.fill(layout);

	vk::DescriptorSetAllocateInfo allocate {};
	allocate.descriptorPool = m_current_pool;
	allocate.pSetLayouts    = layouts.data();

	for (;;) {
		allocate.descriptorSetCount = batch.allocation;
#if defined(KYTY_DESCRIPTOR_HEAP_TELEMETRY) && KYTY_DESCRIPTOR_HEAP_TELEMETRY
		if (m_telemetry_enabled) m_telemetry.AllocateAttempt();
#endif
		const auto result = m_graphics.device.allocateDescriptorSets(&allocate, batch.sets.data());
		if (result == vk::Result::eSuccess) {
			batch.size = batch.allocation;
#if defined(KYTY_DESCRIPTOR_HEAP_TELEMETRY) && KYTY_DESCRIPTOR_HEAP_TELEMETRY
			if (m_telemetry_enabled) {
				m_telemetry.Allocated(batch.allocation);
				m_telemetry.AllocatedTyped(batch.allocation,
				                           batch.diagnostic_types_per_set,
				                           batch.diagnostic_types_known);
			}
#endif
			return true;
		}
		EXIT_IF(result != vk::Result::eErrorOutOfPoolMemory &&
		        result != vk::Result::eErrorFragmentedPool);
#if defined(KYTY_DESCRIPTOR_HEAP_TELEMETRY) && KYTY_DESCRIPTOR_HEAP_TELEMETRY
		if (m_telemetry_enabled)
			m_telemetry.AllocateRejected(result == vk::Result::eErrorFragmentedPool);
#endif
		if (batch.allocation == 1) {
			return false;
		}
		batch.allocation /= 2;
	}
}

void DescriptorHeap::CreateDescriptorPool() {
	vk::DescriptorPoolCreateInfo create {};
	create.maxSets       = DescriptorHeapCount;
	create.poolSizeCount = static_cast<uint32_t>(DescriptorPoolSizes.size());
	create.pPoolSizes    = DescriptorPoolSizes.data();
	EXIT_IF(m_graphics.device.createDescriptorPool(&create, nullptr, &m_current_pool) !=
	        vk::Result::eSuccess);
#if defined(KYTY_DESCRIPTOR_HEAP_TELEMETRY) && KYTY_DESCRIPTOR_HEAP_TELEMETRY
	if (m_telemetry_enabled) m_telemetry.PoolCreated();
#endif
}

} // namespace Libs::Graphics
