#include "model/action.hpp"

namespace model {

bool may_conflict(const Action& lhs, const Action& rhs) {
    if (lhs.address.empty() || rhs.address.empty() || lhs.address != rhs.address) {
        return false;
    }
    return lhs.kind == ActionKind::Write || rhs.kind == ActionKind::Write;
}

bool independent(const Action& lhs, const Action& rhs) {
    // Conflicting memory operations on the same modeled address are dependent:
    // swapping them can change the observed value and whether a race is exposed.
    if (may_conflict(lhs, rhs)) {
        return false;
    }

    const auto lhs_mutex = lhs.kind == ActionKind::Lock || lhs.kind == ActionKind::Unlock;
    const auto rhs_mutex = rhs.kind == ActionKind::Lock || rhs.kind == ActionKind::Unlock;
    if (lhs_mutex && rhs_mutex && lhs.mutex == rhs.mutex) {
        // Operations on one mutex are dependent because ownership, blocking,
        // release/acquire vector-clock state, and future enabledness all depend
        // on their order.
        return false;
    }

    // Remaining pairs commute for this IR when both transitions are enabled:
    // non-conflicting memory accesses update disjoint or read-only memory
    // metadata; mutex operations on distinct mutexes touch disjoint ownership
    // and synchronization clocks; Yield has no modeled shared-state effect.
    // This is intentionally conservative: new action kinds must justify their
    // own commutativity before being classified independent.
    return true;
}

} // namespace model
