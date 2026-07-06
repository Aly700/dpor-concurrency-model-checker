#include "model/action.hpp"

namespace model {
namespace {

bool is_memory_action(const Action& action) {
    return action.kind == ActionKind::Read || action.kind == ActionKind::Write;
}

bool is_mutex_action(const Action& action) {
    return action.kind == ActionKind::Lock ||
           action.kind == ActionKind::Unlock ||
           action.kind == ActionKind::Wait;
}

bool is_condition_action(const Action& action) {
    return action.kind == ActionKind::Wait ||
           action.kind == ActionKind::Signal ||
           action.kind == ActionKind::Broadcast;
}

} // namespace

bool may_conflict(const Action& lhs, const Action& rhs) {
    if (!is_memory_action(lhs) || !is_memory_action(rhs)) {
        return false;
    }
    if (lhs.address.empty() || rhs.address.empty() || lhs.address != rhs.address) {
        return false;
    }
    return lhs.kind == ActionKind::Write || rhs.kind == ActionKind::Write;
}

bool independent(const Action& lhs, const Action& rhs) {
    // Conflicting memory operations on the same modeled address are dependent:
    // swapping them can change the observed value, race endpoint ordering, and
    // resulting read/write metadata. Non-conflicting memory operations commute
    // because they touch disjoint addresses or are both reads, and they do not
    // affect enabledness.
    if (may_conflict(lhs, rhs)) {
        return false;
    }

    if (lhs.kind == ActionKind::Join || rhs.kind == ActionKind::Join) {
        // A precise Join(t) rule needs the owner thread of the other action.
        // The public action-only predicate cannot prove that fact, so every
        // Join pair is conservatively dependent. This over-approximates the
        // required dependency with every action of target thread t and protects
        // enabledness because Join becomes enabled exactly when t is finished.
        return false;
    }

    if (is_condition_action(lhs) && is_condition_action(rhs) && lhs.condition == rhs.condition) {
        // Operations on one condition variable are dependent: Wait mutates the
        // ordered wait set, Signal wakes the lowest-numbered waiter, and
        // Broadcast wakes all waiters. Reordering can change both the resulting
        // wait/woken set and whether future reacquire transitions are enabled.
        return false;
    }

    if (is_mutex_action(lhs) && is_mutex_action(rhs) && lhs.mutex == rhs.mutex) {
        // Operations on one mutex are dependent because ownership, blocking,
        // release/acquire vector-clock state, Wait's atomic release and later
        // reacquire, and future enabledness all depend on their order.
        return false;
    }

    // Remaining pairs commute for this IR when both transitions are enabled:
    // non-conflicting memory accesses update disjoint or read-only memory
    // metadata; mutex and Wait operations on distinct mutexes touch disjoint
    // ownership and synchronization clocks; Signal/Broadcast on different
    // condition variables do not queue permits and mutate disjoint wait sets;
    // Yield has no modeled shared-state or enabledness effect.
    return true;
}

} // namespace model
