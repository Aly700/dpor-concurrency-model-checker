#include "model/action.hpp"

namespace model {
namespace {

bool is_plain_memory_action(const Action& action) {
    return action.kind == ActionKind::Read ||
           action.kind == ActionKind::Write ||
           action.kind == ActionKind::Flush;
}

bool is_atomic_action(const Action& action) {
    return action.kind == ActionKind::AtomicLoad ||
           action.kind == ActionKind::AtomicStore ||
           action.kind == ActionKind::AtomicRmw ||
           action.kind == ActionKind::CompareExchange;
}

bool is_memory_action(const Action& action) {
    return is_plain_memory_action(action) || is_atomic_action(action);
}

bool is_write_like(const Action& action) {
    return action.kind == ActionKind::Write ||
           action.kind == ActionKind::Flush ||
           action.kind == ActionKind::AtomicStore ||
           action.kind == ActionKind::AtomicRmw ||
           action.kind == ActionKind::CompareExchange;
}

bool is_mutex_action(const Action& action) {
    return action.kind == ActionKind::Lock ||
           action.kind == ActionKind::Unlock ||
           action.kind == ActionKind::Wait;
}

bool is_rwlock_action(const Action& action) {
    return action.kind == ActionKind::RLock ||
           action.kind == ActionKind::RUnlock ||
           action.kind == ActionKind::WLock ||
           action.kind == ActionKind::WUnlock;
}

bool is_condition_action(const Action& action) {
    return action.kind == ActionKind::Wait ||
           action.kind == ActionKind::Signal ||
           action.kind == ActionKind::Broadcast;
}

bool is_thread_local_action(const Action& action) {
    return action.kind == ActionKind::Set ||
           action.kind == ActionKind::BranchNonzero ||
           action.kind == ActionKind::Assert ||
           action.kind == ActionKind::Label ||
           action.kind == ActionKind::Fence;
}

} // namespace

bool may_conflict(const Action& lhs, const Action& rhs) {
    if (!is_memory_action(lhs) || !is_memory_action(rhs)) {
        return false;
    }
    if (lhs.address.empty() || rhs.address.empty() || lhs.address != rhs.address) {
        return false;
    }
    if (is_atomic_action(lhs) && is_atomic_action(rhs)) {
        return false;
    }
    return is_write_like(lhs) || is_write_like(rhs);
}

bool independent(const Action& lhs, const Action& rhs) {
    if (is_thread_local_action(lhs) || is_thread_local_action(rhs)) {
        // Register-only operations touch no shared modeled state and do not
        // change another thread's enabledness. Branches can change only their
        // own thread's pc, and same-thread transitions are rejected before the
        // public action predicate is used by DPOR.
        return true;
    }

    if (is_memory_action(lhs) && is_memory_action(rhs) &&
        !lhs.address.empty() && lhs.address == rhs.address) {
        if (is_atomic_action(lhs) && is_atomic_action(rhs)) {
            if (lhs.kind == ActionKind::AtomicLoad && rhs.kind == ActionKind::AtomicLoad) {
                // Two acquire loads of one atomic location are independent:
                // neither mutates the per-location clock, and each only joins
                // that same clock into its own thread. Running them in either
                // order leaves modeled state and enabledness unchanged.
                return true;
            }

            // Atomic Store and RMW pairs on one location are dependent even
            // though atomic-vs-atomic never reports a data race. Their order
            // changes the location clock, which can change downstream HB and
            // mixed plain/atomic race verdicts. Extra independence here would
            // skip a schedule class, so we keep the edge.
            return false;
        }

        if (is_atomic_action(lhs) || is_atomic_action(rhs)) {
            // Any mixed plain/atomic access to one address is dependent. The
            // order determines whether the plain access is HB-ordered with the
            // atomic acquire/release edge before the mixed-race check runs.
            return false;
        }

        if (may_conflict(lhs, rhs)) {
            // Conflicting plain memory operations on the same modeled address
            // are dependent as before: swapping them can change the observed
            // value, race endpoint ordering, and read/write metadata.
            return false;
        }
    }

    if (lhs.kind == ActionKind::Join || rhs.kind == ActionKind::Join) {
        // A precise Join(t) rule needs the owner thread of the other action.
        // The public action-only predicate cannot prove that fact, so every
        // Join pair is conservatively dependent. This over-approximates the
        // required dependency with every action of target thread t and protects
        // enabledness because Join becomes enabled exactly when t is finished.
        return false;
    }

    if (lhs.kind == ActionKind::Spawn || rhs.kind == ActionKind::Spawn) {
        // Spawn creates a start/enabledness edge and copies the spawner's
        // happens-before frontier into the target. The public action-only
        // predicate cannot know which target owns the other action, so every
        // Spawn pair is conservatively dependent.
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

    if (is_rwlock_action(lhs) && is_rwlock_action(rhs) && lhs.rwlock == rhs.rwlock) {
        if (lhs.kind == ActionKind::RLock && rhs.kind == ActionKind::RLock) {
            // For two nonterminal acquisitions that DPOR considers from
            // different threads, both are enabled only while no writer holds
            // this rwlock. The two orders insert the same two reader holders,
            // join the same writer-release clock into separate thread clocks,
            // and leave shared values and race metadata untouched. The final
            // enabled set is identical: in particular, a WLock is disabled
            // after either first RLock and remains disabled after the second.
            // Thus the full commuting diamond, including the potential
            // writer's enabledness, is preserved. A reentrant RLock is also
            // executable so it can report a modeled error; terminal endpoints
            // are protected separately by the checker rule that clears sleep
            // and backtracks every enabled sibling, so this action-level true
            // result never prunes a transition across that terminal outcome.
            return true;
        }

        // All remaining operations on one rwlock start dependent. Besides
        // mutating overlapping ownership/HB state, reader releases can expose
        // a middle writer in only one order. Widening any of these pairs needs
        // both a state diamond and an explicit middle-witness argument.
        return false;
    }

    // Remaining pairs commute for this IR when both transitions are enabled:
    // non-conflicting memory accesses update disjoint or read-only metadata;
    // atomic accesses on different addresses touch disjoint location clocks;
    // mutex and Wait operations on distinct mutexes touch disjoint ownership
    // and synchronization clocks; rwlock operations on distinct rwlocks do
    // likewise; Signal/Broadcast on different condition variables do not
    // queue permits and mutate disjoint wait sets; Yield has no modeled
    // shared-state or enabledness effect.
    return true;
}

} // namespace model
