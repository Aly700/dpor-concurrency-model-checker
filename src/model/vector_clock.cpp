#include "model/vector_clock.hpp"

#include <algorithm>

namespace model {

void VectorClock::tick(std::uint32_t tid) { ++clock_[tid]; }

void VectorClock::join(const VectorClock& other) {
    for (const auto& [tid, value] : other.clock_) {
        clock_[tid] = std::max(clock_[tid], value);
    }
}

bool VectorClock::happens_before_or_equal(const VectorClock& other) const {
    for (const auto& [tid, value] : clock_) {
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

} // namespace model
