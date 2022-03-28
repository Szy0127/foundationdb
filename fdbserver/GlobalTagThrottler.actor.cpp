/*
 * GlobalTagThrottler.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fdbclient/FDBTypes.h"
#include "fdbclient/TagThrottle.actor.h"
#include "fdbrpc/Smoother.h"
#include "fdbserver/TagThrottler.h"

#include <limits>

#include "flow/actorcompiler.h" // must be last include

class GlobalTagThrottlerImpl {
	struct QuotaAndCounters {
		ThrottleApi::TagQuotaValue quota;
		Smoother readCostCounter;
		Smoother writeCostCounter;
		Smoother transactionCounter;

		QuotaAndCounters()
		  : readCostCounter(SERVER_KNOBS->SLOW_SMOOTHING_AMOUNT), writeCostCounter(SERVER_KNOBS->SLOW_SMOOTHING_AMOUNT),
		    transactionCounter(SERVER_KNOBS->SLOW_SMOOTHING_AMOUNT) {}

		ClientTagThrottleLimits getTotalLimit() const {
			auto readLimit =
			    (readCostCounter.smoothRate() == 0)
			        ? std::numeric_limits<double>::max()
			        : quota.totalReadQuota * transactionCounter.smoothRate() / readCostCounter.smoothRate();
			auto writeLimit =
			    (writeCostCounter.smoothRate() == 0)
			        ? std::numeric_limits<double>::max()
			        : quota.totalWriteQuota * transactionCounter.smoothRate() / writeCostCounter.smoothRate();

			// TODO: Implement expiration logic
			return { std::max(SERVER_KNOBS->GLOBAL_TAG_THROTTLING_MIN_RATE, std::min(readLimit, writeLimit)),
				     std::numeric_limits<double>::max() };
		}
	};

	Database db;
	UID id;
	std::map<TransactionTag, QuotaAndCounters> trackedTags;
	uint64_t throttledTagChangeId{ 0 };

	ACTOR static Future<Void> monitorThrottlingChanges(GlobalTagThrottlerImpl* self) {
		loop {
			state ReadYourWritesTransaction tr(self->db);

			loop {
				try {
					tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
					tr.setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);

					state RangeResult currentQuotas = wait(tr.getRange(tagQuotaKeys, CLIENT_KNOBS->TOO_MANY));
					for (auto const kv : currentQuotas) {
						auto tag = kv.key.removePrefix(tagQuotaPrefix);
						auto quota = ThrottleApi::TagQuotaValue::fromValue(kv.value);
						self->trackedTags[tag].quota = quota;
					}

					++self->throttledTagChangeId;
					wait(tr.watch(tagThrottleSignalKey));
					TraceEvent("GlobalTagThrottlerChangeSignaled");
					TEST(true); // Global tag throttler detected quota changes
					break;
				} catch (Error& e) {
					TraceEvent("GlobalTagThrottlerMonitoringChangesError", self->id).error(e);
					wait(tr.onError(e));
				}
			}
		}
	}

public:
	GlobalTagThrottlerImpl(Database db, UID id) : db(db), id(id) {}
	Future<Void> monitorThrottlingChanges() { return monitorThrottlingChanges(this); }
	void addRequests(TransactionTag tag, int count) { trackedTags[tag].transactionCounter.addDelta(count); }
	uint64_t getThrottledTagChangeId() const { return throttledTagChangeId; }
	PrioritizedTransactionTagMap<ClientTagThrottleLimits> getClientRates() {
		// TODO: For now, only enforce total throttling rates.
		// We should use reserved quotas as well.
		PrioritizedTransactionTagMap<ClientTagThrottleLimits> result;
		for (const auto& [tag, quotaAndCounters] : trackedTags) {
			// Currently there is no differentiation between batch priority and default priority transactions
			result[TransactionPriority::BATCH][tag] = result[TransactionPriority::DEFAULT][tag] =
			    quotaAndCounters.getTotalLimit();
		}
		return result;
	}
	int64_t autoThrottleCount() const { return trackedTags.size(); }
	uint32_t busyReadTagCount() const {
		// TODO: Implement
		return 0;
	}
	uint32_t busyWriteTagCount() const {
		// TODO: Implement
		return 0;
	}
	int64_t manualThrottleCount() const { return trackedTags.size(); }
	Future<Void> tryUpdateAutoThrottling(StorageQueueInfo const& ss) {
		for (const auto& busyReadTag : ss.busiestReadTags) {
			trackedTags[busyReadTag.tag].readCostCounter.addDelta(busyReadTag.rate);
		}
		for (const auto& busyWriteTag : ss.busiestWriteTags) {
			trackedTags[busyWriteTag.tag].writeCostCounter.addDelta(busyWriteTag.rate);
		}
		// TODO: Call ThrottleApi::throttleTags
		return Void();
	}
};

GlobalTagThrottler::GlobalTagThrottler(Database db, UID id) : impl(PImpl<GlobalTagThrottlerImpl>::create(db, id)) {}

GlobalTagThrottler::~GlobalTagThrottler() = default;

Future<Void> GlobalTagThrottler::monitorThrottlingChanges() {
	return impl->monitorThrottlingChanges();
}
void GlobalTagThrottler::addRequests(TransactionTag tag, int count) {
	return impl->addRequests(tag, count);
}
uint64_t GlobalTagThrottler::getThrottledTagChangeId() const {
	return impl->getThrottledTagChangeId();
}
PrioritizedTransactionTagMap<ClientTagThrottleLimits> GlobalTagThrottler::getClientRates() {
	return impl->getClientRates();
}
int64_t GlobalTagThrottler::autoThrottleCount() const {
	return impl->autoThrottleCount();
}
uint32_t GlobalTagThrottler::busyReadTagCount() const {
	return impl->busyReadTagCount();
}
uint32_t GlobalTagThrottler::busyWriteTagCount() const {
	return impl->busyWriteTagCount();
}
int64_t GlobalTagThrottler::manualThrottleCount() const {
	return impl->manualThrottleCount();
}
bool GlobalTagThrottler::isAutoThrottlingEnabled() const {
	return true;
}
Future<Void> GlobalTagThrottler::tryUpdateAutoThrottling(StorageQueueInfo const& ss) {
	return impl->tryUpdateAutoThrottling(ss);
}
