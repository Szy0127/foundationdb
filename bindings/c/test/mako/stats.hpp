#ifndef MAKO_STATS_HPP
#define MAKO_STATS_HPP

#include <array>
#include <cstdint>
#include <cstring>
#include <list>
#include <utility>
#include "operations.hpp"
#include "time.hpp"

namespace mako {

/* size of each block to get detailed latency for each operation */
constexpr const size_t LAT_BLOCK_SIZE = 4095;

/* memory block allocated to each operation when collecting detailed latency */
class LatencySampleBlock {
	uint64_t samples[LAT_BLOCK_SIZE]{
		0,
	};
	uint32_t index{ 0 };

public:
	LatencySampleBlock() noexcept = default;
	bool full() const noexcept { return index >= LAT_BLOCK_SIZE; }
	void put(timediff_t td) {
		assert(!full());
		samples[index++] = toIntegerMicroseconds(td);
	}
	// return {data block, number of samples}
	std::pair<uint64_t const*, size_t> data() const noexcept { return { samples, index }; }
};

/* collect sampled latencies */
class LatencySampleBin {
	std::list<LatencySampleBlock> blocks;

public:
	void reserveOneBlock() {
		if (blocks.empty())
			blocks.emplace_back();
	}

	void put(timediff_t td) {
		if (blocks.empty() || blocks.back().full())
			blocks.emplace_back();
		blocks.back().put(td);
	}

	// iterate & apply for each block user function void(uint64_t const*, size_t)
	template <typename Func>
	void forEachBlock(Func&& fn) const {
		for (const auto& block : blocks) {
			auto [ptr, cnt] = block.data();
			fn(ptr, cnt);
		}
	}
};

class alignas(64) ThreadStatistics {
	uint64_t tasks;
	uint64_t conflicts;
	uint64_t total_errors;
	uint64_t ops[MAX_OP];
	uint64_t errors[MAX_OP];
	uint64_t latency_samples[MAX_OP];
	uint64_t latency_us_total[MAX_OP];
	uint64_t latency_us_min[MAX_OP];
	uint64_t latency_us_max[MAX_OP];

public:
	ThreadStatistics() noexcept {
		memset(this, 0, sizeof(ThreadStatistics));
		memset(latency_us_min, 0xff, sizeof(latency_us_min));
	}

	ThreadStatistics(const ThreadStatistics& other) noexcept = default;
	ThreadStatistics& operator=(const ThreadStatistics& other) noexcept = default;

	uint64_t getTaskCount() const noexcept { return tasks; }

	uint64_t getConflictCount() const noexcept { return conflicts; }

	uint64_t getOpCount(int op) const noexcept { return ops[op]; }

	uint64_t getErrorCount(int op) const noexcept { return errors[op]; }

	uint64_t getTotalErrorCount() const noexcept { return total_errors; }

	uint64_t getLatencySampleCount(int op) const noexcept { return latency_samples[op]; }

	uint64_t getLatencyUsTotal(int op) const noexcept { return latency_us_total[op]; }

	uint64_t getLatencyUsMin(int op) const noexcept { return latency_us_min[op]; }

	uint64_t getLatencyUsMax(int op) const noexcept { return latency_us_max[op]; }

	// with 'this' as final aggregation, factor in 'other'
	void combine(const ThreadStatistics& other) {
		tasks += other.tasks;
		conflicts += other.conflicts;
		for (auto op = 0; op < MAX_OP; op++) {
			ops[op] += other.ops[op];
			errors[op] += other.errors[op];
			total_errors += other.errors[op];
			latency_samples[op] += other.latency_samples[op];
			latency_us_total[op] += other.latency_us_total[op];
			if (latency_us_min[op] > other.latency_us_min[op])
				latency_us_min[op] = other.latency_us_min[op];
			if (latency_us_max[op] < other.latency_us_max[op])
				latency_us_max[op] = other.latency_us_max[op];
		}
	}

	void incrTaskCount() noexcept { tasks++; }
	void incrConflictCount() noexcept { conflicts++; }

	// non-commit write operations aren't measured for time.
	void incrOpCount(int op) noexcept { ops[op]++; }

	void incrErrorCount(int op) noexcept {
		total_errors++;
		errors[op]++;
	}

	void addLatency(int op, timediff_t diff) noexcept {
		const auto latency_us = toIntegerMicroseconds(diff);
		latency_samples[op]++;
		latency_us_total[op] += latency_us;
		if (latency_us_min[op] > latency_us)
			latency_us_min[op] = latency_us;
		if (latency_us_max[op] < latency_us)
			latency_us_max[op] = latency_us;
	}
};

using LatencySampleBinArray = std::array<LatencySampleBin, MAX_OP>;

} // namespace mako

#endif /* MAKO_STATS_HPP */
