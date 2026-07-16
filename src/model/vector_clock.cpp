#include "model/vector_clock.hpp"

#if defined(DPOR_EXPLORATION_METRICS)
#include "model/exploration_metrics.hpp"
#endif

#include <algorithm>

namespace model {

void VectorClock::tick(std::uint32_t tid) {
#if defined(DPOR_EXPLORATION_METRICS)
    auto& metrics = diagnostics::detail::mutable_exploration_metrics();
    ++metrics.clock_ticks;
    if (clock_.find(tid) == clock_.end()) {
        ++metrics.clock_map_insertions;
    }
#endif
    ++clock_[tid];
}

void VectorClock::join(const VectorClock& other) {
#if defined(DPOR_EXPLORATION_METRICS)
    auto& metrics = diagnostics::detail::mutable_exploration_metrics();
    ++metrics.clock_joins;
    metrics.clock_join_components += other.clock_.size();
#endif
    for (const auto& [tid, value] : other.clock_) {
#if defined(DPOR_EXPLORATION_METRICS)
        if (clock_.find(tid) == clock_.end()) {
            ++metrics.clock_map_insertions;
        }
#endif
        clock_[tid] = std::max(clock_[tid], value);
    }
}

bool VectorClock::happens_before_or_equal(const VectorClock& other) const {
#if defined(DPOR_EXPLORATION_METRICS)
    auto& metrics = diagnostics::detail::mutable_exploration_metrics();
    ++metrics.clock_comparisons;
#endif
    for (const auto& [tid, value] : clock_) {
#if defined(DPOR_EXPLORATION_METRICS)
        ++metrics.clock_compare_components;
#endif
        if (value > other.get(tid)) {
            return false;
        }
    }
    return true;
}

std::uint64_t VectorClock::get(std::uint32_t tid) const {
    auto it = clock_.find(tid);
    return it == clock_.end() ? 0 : it->second;
}

std::size_t VectorClock::diagnostic_component_count() const { return clock_.size(); }

} // namespace model
