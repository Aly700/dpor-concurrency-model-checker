#include "model/checker.hpp"

#include "model/vector_clock.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace model {
namespace {

struct MemoryAccess {
    VectorClock clock;
    ScheduleStep endpoint;
    bool is_atomic{false};
    bool is_write{false};
};

struct AddressState {
    std::optional<MemoryAccess> last_write;
    std::map<std::pair<ThreadId, std::uint32_t>, MemoryAccess> reads_since_last_write;
    std::vector<MemoryAccess> plain_accesses;
    std::vector<MemoryAccess> atomic_accesses;
};

enum class WaitPhase { None, Waiting, Woken };

struct ExecutionState {
    std::vector<std::uint32_t> pc;
    std::map<std::string, ThreadId> mutex_owner;
    std::map<std::string, VectorClock> mutex_clock;
    std::map<std::string, std::vector<ThreadId>> condition_waiters;
    std::vector<VectorClock> thread_clock;
    std::vector<WaitPhase> wait_phase;
    std::map<std::string, AddressState> memory;
    std::map<std::string, VectorClock> atomic_location_clock;
    Schedule schedule;
};

struct StepReport {
    std::optional<RaceReport> race;
    std::optional<ModelErrorReport> error;
};

struct EnabledTransition {
    ScheduleStep endpoint;
    Action effective_action;
};

struct DporNode {
    std::vector<ThreadId> enabled;
    std::map<ThreadId, EnabledTransition> enabled_transitions;
    std::vector<ThreadId> backtrack;
    std::vector<ThreadId> done;
    std::vector<ThreadId> sleep;
};

struct ExecutedTransition {
    ThreadId thread{0};
    Action effective_action;
    ScheduleStep endpoint;
    VectorClock clock;
};

ExecutionState initial_state(const Program& program) {
    return ExecutionState{
        std::vector<std::uint32_t>(program.threads.size(), 0),
        {},
        {},
        {},
        std::vector<VectorClock>(program.threads.size()),
        std::vector<WaitPhase>(program.threads.size(), WaitPhase::None),
        {},
        {},
        {},
    };
}

bool is_finished(const Program& program, const ExecutionState& state, ThreadId tid) {
    return state.pc.at(tid) >= program.threads.at(tid).size();
}

bool all_finished(const Program& program, const ExecutionState& state) {
    for (ThreadId tid = 0; tid < program.threads.size(); ++tid) {
        if (!is_finished(program, state, tid)) {
            return false;
        }
    }
    return true;
}

const Action& next_action(const Program& program, const ExecutionState& state, ThreadId tid) {
    return program.threads.at(tid).at(state.pc.at(tid));
}

Action effective_next_action(const Program& program, const ExecutionState& state, ThreadId tid) {
    Action action = next_action(program, state, tid);
    if (action.kind == ActionKind::Wait && state.wait_phase.at(tid) == WaitPhase::Woken) {
        // Wait is deliberately phase-aware for DPOR. The release/sleep phase
        // mutates the condition wait set and releases the mutex, while the
        // later woken phase is only the mutex reacquire. Keying sleep-set or
        // backtracking dependence on the static Wait action would incorrectly
        // make the reacquire look dependent with later Signal/Broadcast
        // operations on the same cv; replay still records the original
        // (thread, action_index), but reduction uses this effective Lock.
        Action reacquire;
        reacquire.kind = ActionKind::Lock;
        reacquire.mutex = action.mutex;
        return reacquire;
    }
    return action;
}

bool owns_mutex(const ExecutionState& state, ThreadId tid, const std::string& mutex) {
    const auto owner = state.mutex_owner.find(mutex);
    return owner != state.mutex_owner.end() && owner->second == tid;
}

bool join_target_is_invalid(const Program& program, ThreadId tid, const Action& action) {
    return action.target >= program.threads.size() || action.target == tid;
}

bool is_enabled(const Program& program, const ExecutionState& state, ThreadId tid) {
    if (is_finished(program, state, tid)) {
        return false;
    }

    const Action& action = next_action(program, state, tid);
    switch (action.kind) {
    case ActionKind::Lock:
        return state.mutex_owner.find(action.mutex) == state.mutex_owner.end();
    case ActionKind::Join:
        if (join_target_is_invalid(program, tid, action)) {
            return true;
        }
        return is_finished(program, state, action.target);
    case ActionKind::Wait:
        if (state.wait_phase.at(tid) == WaitPhase::Waiting) {
            return false;
        }
        if (state.wait_phase.at(tid) == WaitPhase::Woken) {
            // INVARIANTS.md Replay/HB: a woken Wait replays as the same
            // action index, but it remains disabled until the mutex reacquire
            // edge can be applied exactly like Lock.
            return state.mutex_owner.find(action.mutex) == state.mutex_owner.end();
        }
        return true;
    case ActionKind::Read:
    case ActionKind::Write:
    case ActionKind::AtomicLoad:
    case ActionKind::AtomicStore:
    case ActionKind::AtomicRmw:
    case ActionKind::Unlock:
    case ActionKind::Signal:
    case ActionKind::Broadcast:
    case ActionKind::Yield:
        return true;
    }

    return false;
}

bool any_enabled(const Program& program, const ExecutionState& state) {
    for (ThreadId tid = 0; tid < program.threads.size(); ++tid) {
        if (is_enabled(program, state, tid)) {
            return true;
        }
    }
    return false;
}

std::vector<ThreadId> enabled_threads(const Program& program, const ExecutionState& state) {
    std::vector<ThreadId> enabled;
    for (ThreadId tid = 0; tid < program.threads.size(); ++tid) {
        // INVARIANTS.md Replay/HB: Wait's two phases are represented in
        // per-thread state, not by rewriting the program. A sleeping waiter is
        // absent, and a woken waiter appears only when the mutex reacquire edge
        // can run under the original (thread, action_index) schedule step.
        // Join appears only when its target is finished, except invalid joins
        // stay enabled so replay reaches the modeled error deterministically.
        if (is_enabled(program, state, tid)) {
            enabled.push_back(tid);
        }
    }
    return enabled;
}

std::map<ThreadId, EnabledTransition> enabled_transitions(const Program& program,
                                                          const ExecutionState& state) {
    std::map<ThreadId, EnabledTransition> transitions;
    for (ThreadId tid = 0; tid < program.threads.size(); ++tid) {
        if (!is_enabled(program, state, tid)) {
            continue;
        }
        transitions.emplace(
            tid,
            EnabledTransition{
                ScheduleStep{tid, state.pc.at(tid)},
                effective_next_action(program, state, tid),
            });
    }
    return transitions;
}

bool contains_thread(const std::vector<ThreadId>& threads, ThreadId tid) {
    return std::binary_search(threads.begin(), threads.end(), tid);
}

void insert_thread(std::vector<ThreadId>& threads, ThreadId tid) {
    const auto position = std::lower_bound(threads.begin(), threads.end(), tid);
    if (position == threads.end() || *position != tid) {
        threads.insert(position, tid);
    }
}

bool transition_enabled_at_node(const DporNode& node, const ExecutedTransition& transition) {
    const auto enabled = node.enabled_transitions.find(transition.thread);
    return enabled != node.enabled_transitions.end() &&
           enabled->second.endpoint == transition.endpoint &&
           enabled->second.effective_action == transition.effective_action;
}

std::optional<ThreadId> next_unexplored_backtrack(const DporNode& node) {
    for (const ThreadId tid : node.backtrack) {
        if (!contains_thread(node.done, tid)) {
            return tid;
        }
    }
    return std::nullopt;
}

bool ordered_by_happens_before(const MemoryAccess& lhs, const MemoryAccess& rhs) {
    return lhs.clock.happens_before_or_equal(rhs.clock) || rhs.clock.happens_before_or_equal(lhs.clock);
}

RaceReport make_race_report(const std::string& address,
                            const MemoryAccess& prior,
                            const MemoryAccess& current,
                            const Schedule& schedule) {
    return RaceReport{address, prior.endpoint, current.endpoint, schedule};
}

std::optional<RaceReport> record_read(ExecutionState& state,
                                      const Action& action,
                                      const MemoryAccess& current) {
    auto& address_state = state.memory[action.address];
    std::optional<RaceReport> race;
    if (address_state.last_write.has_value() &&
        !ordered_by_happens_before(*address_state.last_write, current)) {
        race = make_race_report(action.address, *address_state.last_write, current, state.schedule);
    }
    address_state.reads_since_last_write.emplace(
        std::make_pair(current.endpoint.thread, current.endpoint.action_index), current);
    if (!race.has_value()) {
        for (const auto& atomic : address_state.atomic_accesses) {
            if ((atomic.is_write || current.is_write) &&
                !ordered_by_happens_before(atomic, current)) {
                race = make_race_report(action.address, atomic, current, state.schedule);
                break;
            }
        }
    }
    address_state.plain_accesses.push_back(current);
    return race;
}

std::optional<RaceReport> record_write(ExecutionState& state,
                                       const Action& action,
                                       const MemoryAccess& current) {
    auto& address_state = state.memory[action.address];
    std::optional<RaceReport> race;
    if (address_state.last_write.has_value() &&
        !ordered_by_happens_before(*address_state.last_write, current)) {
        race = make_race_report(action.address, *address_state.last_write, current, state.schedule);
    }

    if (!race.has_value()) {
        for (const auto& [_, read] : address_state.reads_since_last_write) {
            if (!ordered_by_happens_before(read, current)) {
                race = make_race_report(action.address, read, current, state.schedule);
                break;
            }
        }
    }

    if (!race.has_value()) {
        for (const auto& atomic : address_state.atomic_accesses) {
            if ((atomic.is_write || current.is_write) &&
                !ordered_by_happens_before(atomic, current)) {
                race = make_race_report(action.address, atomic, current, state.schedule);
                break;
            }
        }
    }

    address_state.last_write = current;
    address_state.reads_since_last_write.clear();
    address_state.plain_accesses.push_back(current);
    return race;
}

std::optional<RaceReport> record_atomic(ExecutionState& state,
                                        const Action& action,
                                        const MemoryAccess& current) {
    auto& address_state = state.memory[action.address];
    std::optional<RaceReport> race;
    for (const auto& plain : address_state.plain_accesses) {
        if ((plain.is_write || current.is_write) &&
            !ordered_by_happens_before(plain, current)) {
            race = make_race_report(action.address, plain, current, state.schedule);
            break;
        }
    }
    address_state.atomic_accesses.push_back(current);
    return race;
}

ModelErrorReport make_unlock_error(const Action& action,
                                   ScheduleStep endpoint,
                                   const Schedule& schedule,
                                   const ExecutionState& state) {
    std::ostringstream message;
    message << "thread " << endpoint.thread << " attempted to unlock mutex '" << action.mutex << "'";
    const auto owner = state.mutex_owner.find(action.mutex);
    if (owner == state.mutex_owner.end()) {
        message << " but it is not owned";
    } else {
        message << " owned by thread " << owner->second;
    }
    return ModelErrorReport{endpoint, message.str(), schedule};
}

ModelErrorReport make_join_error(const Action& action, ScheduleStep endpoint, const Schedule& schedule) {
    std::ostringstream message;
    message << "thread " << endpoint.thread << " attempted to join ";
    if (action.target == endpoint.thread) {
        message << "itself";
    } else {
        message << "out-of-range thread " << action.target;
    }
    return ModelErrorReport{endpoint, message.str(), schedule};
}

ModelErrorReport make_wait_error(const Action& action,
                                 ScheduleStep endpoint,
                                 const Schedule& schedule,
                                 const ExecutionState& state) {
    std::ostringstream message;
    message << "thread " << endpoint.thread << " attempted to wait on condition '"
            << action.condition << "' with mutex '" << action.mutex << "'";
    const auto owner = state.mutex_owner.find(action.mutex);
    if (owner == state.mutex_owner.end()) {
        message << " but the mutex is not owned";
    } else {
        message << " but it is owned by thread " << owner->second;
    }
    return ModelErrorReport{endpoint, message.str(), schedule};
}

void insert_waiter(ExecutionState& state, const std::string& condition, ThreadId tid) {
    auto& waiters = state.condition_waiters[condition];
    const auto position = std::lower_bound(waiters.begin(), waiters.end(), tid);
    assert(position == waiters.end() || *position != tid);
    waiters.insert(position, tid);
}

void wake_waiter(ExecutionState& state, ThreadId signaler, ThreadId waiter) {
    assert(state.wait_phase.at(waiter) == WaitPhase::Waiting);
    state.wait_phase.at(waiter) = WaitPhase::Woken;
    state.thread_clock.at(waiter).join(state.thread_clock.at(signaler));
}

void signal_one_waiter(ExecutionState& state, const Action& action, ThreadId signaler) {
    auto& waiters = state.condition_waiters[action.condition];
    if (waiters.empty()) {
        return;
    }

    const ThreadId waiter = waiters.front();
    waiters.erase(waiters.begin());
    wake_waiter(state, signaler, waiter);
}

void broadcast_waiters(ExecutionState& state, const Action& action, ThreadId signaler) {
    auto& waiters = state.condition_waiters[action.condition];
    const std::vector<ThreadId> to_wake = waiters;
    waiters.clear();
    for (const ThreadId waiter : to_wake) {
        wake_waiter(state, signaler, waiter);
    }
}

StepReport execute_enabled_step(const Program& program, ExecutionState& state, ThreadId tid) {
    const auto action_index = state.pc.at(tid);
    const ScheduleStep endpoint{tid, action_index};
    const Action& action = program.threads.at(tid).at(action_index);

    state.schedule.push_back(endpoint);
    state.thread_clock.at(tid).tick(tid);

    StepReport report;
    switch (action.kind) {
    case ActionKind::Lock:
        ++state.pc.at(tid);
        state.mutex_owner[action.mutex] = tid;
        state.thread_clock.at(tid).join(state.mutex_clock[action.mutex]);
        break;
    case ActionKind::Join:
        ++state.pc.at(tid);
        if (join_target_is_invalid(program, tid, action)) {
            report.error = make_join_error(action, endpoint, state.schedule);
            break;
        }
        state.thread_clock.at(tid).join(state.thread_clock.at(action.target));
        break;
    case ActionKind::Unlock: {
        ++state.pc.at(tid);
        const auto owner = state.mutex_owner.find(action.mutex);
        if (owner == state.mutex_owner.end() || owner->second != tid) {
            report.error = make_unlock_error(action, endpoint, state.schedule, state);
            break;
        }
        state.mutex_clock[action.mutex] = state.thread_clock.at(tid);
        state.mutex_owner.erase(owner);
        break;
    }
    case ActionKind::Wait: {
        if (state.wait_phase.at(tid) == WaitPhase::Woken) {
            assert(state.mutex_owner.find(action.mutex) == state.mutex_owner.end());
            state.mutex_owner[action.mutex] = tid;
            state.thread_clock.at(tid).join(state.mutex_clock[action.mutex]);
            state.wait_phase.at(tid) = WaitPhase::None;
            ++state.pc.at(tid);
            break;
        }

        assert(state.wait_phase.at(tid) == WaitPhase::None);
        if (!owns_mutex(state, tid, action.mutex)) {
            ++state.pc.at(tid);
            report.error = make_wait_error(action, endpoint, state.schedule, state);
            break;
        }

        state.mutex_clock[action.mutex] = state.thread_clock.at(tid);
        state.mutex_owner.erase(action.mutex);
        insert_waiter(state, action.condition, tid);
        state.wait_phase.at(tid) = WaitPhase::Waiting;
        break;
    }
    case ActionKind::Signal:
        ++state.pc.at(tid);
        signal_one_waiter(state, action, tid);
        break;
    case ActionKind::Broadcast:
        ++state.pc.at(tid);
        broadcast_waiters(state, action, tid);
        break;
    case ActionKind::AtomicLoad:
        ++state.pc.at(tid);
        if (!action.address.empty()) {
            // Acquire load: join the thread with exactly this location's last
            // release sequence clock. Missing this edge over-reports races
            // after a real synchronizes-with relation; adding any extra edge
            // can hide a real plain-data race, so the model only joins the
            // per-address atomic clock and does not mutate it.
            state.thread_clock.at(tid).join(state.atomic_location_clock[action.address]);
            report.race = record_atomic(
                state,
                action,
                MemoryAccess{state.thread_clock.at(tid), endpoint, true, false});
        }
        break;
    case ActionKind::AtomicStore:
        ++state.pc.at(tid);
        if (!action.address.empty()) {
            // Release store: replace the location clock with the storing
            // thread's post-tick clock. It must not join the previous
            // location clock; such an extra HB edge would fabricate ordering
            // from an earlier other-thread store to a later load and could
            // suppress a real plain-data race. The store itself performs no
            // acquire join, intentionally erring toward fewer edges.
            state.atomic_location_clock[action.address] = state.thread_clock.at(tid);
            report.race = record_atomic(
                state,
                action,
                MemoryAccess{state.thread_clock.at(tid), endpoint, true, true});
        }
        break;
    case ActionKind::AtomicRmw:
        ++state.pc.at(tid);
        if (!action.address.empty()) {
            // Acq_rel RMW: first acquire from the current location clock, then
            // replace the location clock with the joined thread clock. The
            // acquire half preserves the C++ release-sequence edge; the
            // replace half avoids accumulating unrelated previous stores. When
            // forced to choose, the model avoids extra HB edges because they
            // hide races, while missing edges only over-report.
            state.thread_clock.at(tid).join(state.atomic_location_clock[action.address]);
            state.atomic_location_clock[action.address] = state.thread_clock.at(tid);
            report.race = record_atomic(
                state,
                action,
                MemoryAccess{state.thread_clock.at(tid), endpoint, true, true});
        }
        break;
    case ActionKind::Read:
        ++state.pc.at(tid);
        if (!action.address.empty()) {
            report.race = record_read(
                state,
                action,
                MemoryAccess{state.thread_clock.at(tid), endpoint, false, false});
        }
        break;
    case ActionKind::Write:
        ++state.pc.at(tid);
        if (!action.address.empty()) {
            report.race = record_write(
                state,
                action,
                MemoryAccess{state.thread_clock.at(tid), endpoint, false, true});
        }
        break;
    case ActionKind::Yield:
        ++state.pc.at(tid);
        break;
    }
    return report;
}

DeadlockReport make_deadlock_report(const Program& program, const ExecutionState& state) {
    DeadlockReport report;
    for (ThreadId tid = 0; tid < program.threads.size(); ++tid) {
        if (is_finished(program, state, tid)) {
            continue;
        }

        const Action& action = next_action(program, state, tid);
        // INVARIANTS.md Soundness/Replay: a terminal state with unfinished
        // disabled threads must describe the exact blocker visible to replay.
        // Lock and woken-Wait reacquire wait on a mutex, Join waits on its
        // target thread to finish, and sleeping Wait waits on its condition
        // variable without inventing a queued permit.
        if (action.kind == ActionKind::Lock) {
            const auto owner = state.mutex_owner.find(action.mutex);
            BlockedThread blocked;
            blocked.thread = tid;
            blocked.kind = BlockedOnKind::Mutex;
            blocked.mutex = action.mutex;
            blocked.owner = owner == state.mutex_owner.end()
                                ? std::optional<ThreadId>{}
                                : std::optional<ThreadId>{owner->second};
            report.blocked_threads.push_back(std::move(blocked));
        } else if (action.kind == ActionKind::Join && !join_target_is_invalid(program, tid, action)) {
            BlockedThread blocked;
            blocked.thread = tid;
            blocked.kind = BlockedOnKind::Thread;
            blocked.target = action.target;
            report.blocked_threads.push_back(std::move(blocked));
        } else if (action.kind == ActionKind::Wait) {
            if (state.wait_phase.at(tid) == WaitPhase::Waiting) {
                BlockedThread blocked;
                blocked.thread = tid;
                blocked.kind = BlockedOnKind::ConditionVariable;
                blocked.condition = action.condition;
                blocked.mutex = action.mutex;
                report.blocked_threads.push_back(std::move(blocked));
            } else if (state.wait_phase.at(tid) == WaitPhase::Woken) {
                const auto owner = state.mutex_owner.find(action.mutex);
                BlockedThread blocked;
                blocked.thread = tid;
                blocked.kind = BlockedOnKind::Mutex;
                blocked.mutex = action.mutex;
                blocked.owner = owner == state.mutex_owner.end()
                                    ? std::optional<ThreadId>{}
                                    : std::optional<ThreadId>{owner->second};
                report.blocked_threads.push_back(std::move(blocked));
            }
        }
    }
    report.schedule = state.schedule;
    return report;
}

void record_step_report(CheckResult& result, const StepReport& report) {
    if (report.error.has_value() && !result.first_error.has_value()) {
        result.first_error = report.error;
    }
    if (report.race.has_value() && !result.first_race.has_value()) {
        result.first_race = report.race;
    }
}

void initialize_dpor_backtrack(const Program& program, const ExecutionState& state, DporNode& node) {
    if (!node.backtrack.empty() || node.enabled.empty()) {
        return;
    }

    const auto first_awake = std::find_if(
        node.enabled.begin(),
        node.enabled.end(),
        [&](ThreadId tid) { return !contains_thread(node.sleep, tid); });
    if (first_awake == node.enabled.end()) {
        // Sleep sets interact with the max_schedules cutoff only by reducing
        // the number of representative schedules counted. If every enabled
        // transition at this prefix is asleep, the prefix is Mazurkiewicz-
        // equivalent to one already explored and contributes no schedule.
        return;
    }

    insert_thread(node.backtrack, *first_awake);

    bool changed = true;
    while (changed) {
        changed = false;
        for (const ThreadId candidate : node.enabled) {
            if (contains_thread(node.backtrack, candidate) ||
                contains_thread(node.sleep, candidate)) {
                continue;
            }

            for (const ThreadId selected : node.backtrack) {
                if (!independent(effective_next_action(program, state, candidate),
                                 effective_next_action(program, state, selected))) {
                    // INVARIANTS.md Soundness/Independence: an enabled
                    // transition is pruned from the initial persistent set
                    // only when independent() says it commutes with every
                    // selected enabled transition. A dependent enabled
                    // transition is kept so a distinct bug class is not
                    // skipped.
                    insert_thread(node.backtrack, candidate);
                    changed = true;
                    break;
                }
            }
        }
    }
}

void add_backtracks_for_transition_against_prefix(std::vector<DporNode>& nodes,
                                                  const std::vector<ExecutedTransition>& trace,
                                                  const ExecutedTransition& current,
                                                  std::size_t prefix_size) {
    std::optional<std::size_t> earliest_disabled_dependent;
    for (std::size_t index = prefix_size; index > 0; --index) {
        const std::size_t previous_index = index - 1;
        const ExecutedTransition& previous = trace.at(previous_index);
        if (previous.thread == current.thread) {
            // INVARIANTS.md Replay: schedules preserve per-thread action-index
            // order, so same-thread transitions cannot be swapped into a new
            // legal replay schedule.
            continue;
        }

        if (independent(previous.effective_action, current.effective_action)) {
            // INVARIANTS.md Soundness/Independence: this is the DPOR pruning
            // predicate. We may commute and therefore avoid a backtrack only
            // when independent() says ordering cannot affect observable state
            // or future enabledness.
            continue;
        }

        DporNode& backtrack_point = nodes.at(previous_index);
        if (!transition_enabled_at_node(backtrack_point, current)) {
            // Disabled-transition fallback is about reaching the later
            // effective transition at all, not just reversing two already
            // enabled transitions. A thread may be enabled here with an
            // earlier action, or a Wait may be enabled in the opposite phase;
            // in both cases an HB edge observed in this trace can disappear
            // after those prerequisites run before the earlier transition.
            // Keep the earliest dependent repair point so lock-order,
            // Join-enabledness, and cv wakeup bugs are not pruned.
            earliest_disabled_dependent = previous_index;
            continue;
        }

        if (previous.clock.happens_before_or_equal(current.clock)) {
            // INVARIANTS.md Soundness/HB: an HB-ordered pair cannot be reversed
            // by any schedule in any Mazurkiewicz class reachable from this
            // prefix, so skipping this backtrack loses no race/deadlock/error
            // class. Signal/Broadcast wake edges are already joined into the
            // woken thread before its reacquire transition is recorded, so a
            // reacquire HB-after its concrete waker is safely skipped; other
            // possible waiter-set/waker orders are protected earlier because
            // Wait and Signal/Broadcast on the same cv remain dependent.
            // Join is similarly safe: after a successful Join, the joiner's
            // clock includes the target's final clock, so target actions that
            // are HB-before the Join do not need reversal backtracks.
            continue;
        }

        // Flanagan-Godefroid DPOR adds the later thread at the last dependent,
        // non-HB-ordered prefix only. Earlier required reversal points are
        // added inductively when exploration reaches those prefixes, which
        // avoids the old conservative "every dependent prefix" explosion
        // without weakening INVARIANTS.md Soundness.
        insert_thread(backtrack_point.backtrack, current.thread);
        return;
    }

    if (earliest_disabled_dependent.has_value()) {
        DporNode& backtrack_point = nodes.at(*earliest_disabled_dependent);
        // INVARIANTS.md Soundness, highest-risk disabled-transition fallback:
        // the later effective transition could not be scheduled at this
        // earlier point, so add every enabled thread. This preserves deadlock
        // and pre-error detection by exploring whichever enabled transition
        // may make the dependent transition reachable before the earlier one.
        for (const ThreadId enabled : backtrack_point.enabled) {
            insert_thread(backtrack_point.backtrack, enabled);
        }
    }
}

void add_backtracks_for_transition(std::vector<DporNode>& nodes,
                                   const std::vector<ExecutedTransition>& trace) {
    if (trace.empty()) {
        return;
    }

    add_backtracks_for_transition_against_prefix(nodes, trace, trace.back(), trace.size() - 1);
}

void add_disabled_backtracks(const Program& program,
                             const ExecutionState& state,
                             std::vector<DporNode>& nodes,
                             const std::vector<ExecutedTransition>& trace) {
    for (ThreadId tid = 0; tid < program.threads.size(); ++tid) {
        if (is_finished(program, state, tid) || is_enabled(program, state, tid)) {
            continue;
        }

        const ExecutedTransition blocked{
            tid,
            effective_next_action(program, state, tid),
            ScheduleStep{tid, state.pc.at(tid)},
            state.thread_clock.at(tid),
        };
        // INVARIANTS.md Soundness/Deadlock: a blocked Lock, Join, sleeping
        // Wait, or woken-Wait reacquire absent from the executed trace may be
        // exactly the dependent action needed to expose another deadlock,
        // race, or error schedule. We therefore apply the same
        // independent()-guarded backtrack rule to disabled next actions at
        // terminal leaves; independent blocked actions remain pruned only when
        // the Independence invariant permits commuting them. effective_next_action()
        // makes the woken-Wait case a mutex reacquire, not a cv wait, while a
        // still-sleeping waiter remains condition-dependent for wakeup order.
        add_backtracks_for_transition_against_prefix(nodes, trace, blocked, trace.size());
    }
}

std::vector<ThreadId> inherited_sleep_set(const Program& program,
                                          const ExecutionState& state_after_transition,
                                          const DporNode& parent,
                                          const ExecutedTransition& transition) {
    std::vector<ThreadId> inherited;
    for (const ThreadId tid : parent.sleep) {
        if (tid == transition.thread || !is_enabled(program, state_after_transition, tid)) {
            continue;
        }

        const Action slept_action = effective_next_action(program, state_after_transition, tid);
        if (independent(slept_action, transition.effective_action)) {
            // Classic Godefroid sleep-set propagation: a slept transition is
            // inherited only while its phase-aware next action still commutes
            // with the transition just executed. If it is dependent, it must
            // be removed so the child prefix can keep a representative for any
            // newly distinct Mazurkiewicz class.
            insert_thread(inherited, tid);
        }
    }
    return inherited;
}

void dpor_dfs(const Program& program,
              ExecutionState state,
              CheckResult& result,
              std::size_t max_schedules,
              std::vector<DporNode>& nodes,
              std::vector<ExecutedTransition>& trace,
              std::vector<ThreadId> sleep_set) {
    if (result.schedules_explored >= max_schedules) {
        return;
    }

    const auto depth = nodes.size();
    nodes.push_back(DporNode{
        enabled_threads(program, state),
        enabled_transitions(program, state),
        {},
        {},
        std::move(sleep_set),
    });

    if (nodes.at(depth).enabled.empty()) {
        ++result.schedules_explored;
        if (!all_finished(program, state)) {
            add_disabled_backtracks(program, state, nodes, trace);
        }
        if (!all_finished(program, state) && !result.first_deadlock.has_value()) {
            result.first_deadlock = make_deadlock_report(program, state);
        }
        nodes.pop_back();
        return;
    }

    initialize_dpor_backtrack(program, state, nodes.at(depth));
    if (nodes.at(depth).backtrack.empty()) {
        if (!all_finished(program, state)) {
            // A sleep-blocked prefix is equivalent to an explored execution
            // only for the enabled transitions it would run. Disabled Lock,
            // Join, and Wait-reacquire transitions can still be the evidence
            // that an earlier enabledness repair is needed, especially before
            // a slept modeled-error endpoint. Apply the terminal disabled
            // fallback before pruning the slept representative.
            add_disabled_backtracks(program, state, nodes, trace);
        }
        nodes.pop_back();
        return;
    }

    while (result.schedules_explored < max_schedules) {
        const std::optional<ThreadId> next_tid = next_unexplored_backtrack(nodes.at(depth));
        if (!next_tid.has_value()) {
            break;
        }

        insert_thread(nodes.at(depth).done, *next_tid);
        if (!contains_thread(nodes.at(depth).enabled, *next_tid)) {
            continue;
        }
        if (contains_thread(nodes.at(depth).sleep, *next_tid)) {
            // Sleep-blocked prefixes are not counted as explored schedules:
            // the schedule budget applies to representatives that actually
            // execute. Choice order remains deterministic because slept
            // backtrack entries are skipped in ascending thread-id order.
            continue;
        }

        const auto action_index = state.pc.at(*next_tid);
        const Action effective_action = effective_next_action(program, state, *next_tid);

        ExecutionState next = state;
        const StepReport step_report = execute_enabled_step(program, next, *next_tid);
        const ExecutedTransition transition{
            *next_tid,
            effective_action,
            ScheduleStep{*next_tid, action_index},
            next.thread_clock.at(*next_tid),
        };
        trace.push_back(transition);
        add_backtracks_for_transition(nodes, trace);
        record_step_report(result, step_report);

        if (step_report.error.has_value()) {
            add_disabled_backtracks(program, state, nodes, trace);
            // INVARIANTS.md Soundness: a modeled error terminates this
            // schedule, so even independent enabled siblings cannot be
            // represented by running them after the error. Do not prune at
            // this node after an error endpoint; add every enabled sibling so
            // races, deadlocks, or other errors reachable before this error
            // still have a representative schedule.
            nodes.at(depth).sleep.clear();
            for (const ThreadId enabled : nodes.at(depth).enabled) {
                insert_thread(nodes.at(depth).backtrack, enabled);
            }
            ++result.schedules_explored;
        } else {
            // INVARIANTS.md Soundness/Independence: enabled transitions not in
            // this node's backtrack set are the only schedules pruned here.
            // The initial persistent set and dynamic backtrack additions above
            // omit an alternative solely after independent() justifies
            // commuting it with the representative transition.
            std::vector<ThreadId> child_sleep =
                inherited_sleep_set(program, next, nodes.at(depth), transition);
            dpor_dfs(program,
                     std::move(next),
                     result,
                     max_schedules,
                     nodes,
                     trace,
                     std::move(child_sleep));
        }

        trace.pop_back();
        insert_thread(nodes.at(depth).sleep, *next_tid);
    }

    nodes.pop_back();
}

void dfs(const Program& program,
         ExecutionState state,
         CheckResult& result,
         std::size_t max_schedules) {
    if (result.schedules_explored >= max_schedules) {
        return;
    }

    bool explored_child = false;
    for (ThreadId tid = 0; tid < program.threads.size(); ++tid) {
        if (!is_enabled(program, state, tid)) {
            continue;
        }

        explored_child = true;
        ExecutionState next = state;
        const StepReport step_report = execute_enabled_step(program, next, tid);
        record_step_report(result, step_report);
        if (step_report.error.has_value()) {
            ++result.schedules_explored;
        } else {
            dfs(program, std::move(next), result, max_schedules);
        }

        if (result.schedules_explored >= max_schedules) {
            return;
        }
    }

    if (explored_child) {
        return;
    }

    ++result.schedules_explored;
    if (!all_finished(program, state) && !result.first_deadlock.has_value()) {
        result.first_deadlock = make_deadlock_report(program, state);
    }
}

std::invalid_argument invalid_schedule(std::size_t index, const std::string& reason) {
    std::ostringstream message;
    message << "invalid replay schedule at step " << index << ": " << reason;
    return std::invalid_argument(message.str());
}

void validate_replay_step(const Program& program,
                          const ExecutionState& state,
                          const ScheduleStep& step,
                          std::size_t step_index) {
    if (step.thread >= program.threads.size()) {
        std::ostringstream reason;
        reason << "schedule names out-of-range thread " << step.thread;
        throw invalid_schedule(step_index, reason.str());
    }

    const auto& thread = program.threads.at(step.thread);
    if (step.action_index >= thread.size()) {
        std::ostringstream reason;
        reason << "schedule names out-of-range action " << step.action_index
               << " for thread " << step.thread;
        throw invalid_schedule(step_index, reason.str());
    }

    const auto expected_action = state.pc.at(step.thread);
    if (step.action_index != expected_action) {
        std::ostringstream reason;
        reason << "schedule names action " << step.action_index << " for thread " << step.thread
               << " but the next action is " << expected_action;
        throw invalid_schedule(step_index, reason.str());
    }

    if (!is_enabled(program, state, step.thread)) {
        std::ostringstream reason;
        reason << "schedule names a disabled action for thread " << step.thread;
        throw invalid_schedule(step_index, reason.str());
    }
}

CheckResult replay_schedule(const Program& program, const Schedule& schedule) {
    CheckResult result;
    ExecutionState state = initial_state(program);

    for (std::size_t i = 0; i < schedule.size(); ++i) {
        validate_replay_step(program, state, schedule[i], i);
        const StepReport step_report = execute_enabled_step(program, state, schedule[i].thread);
        record_step_report(result, step_report);
        if (step_report.error.has_value()) {
            if (i + 1 < schedule.size()) {
                throw invalid_schedule(i + 1, "schedule continues after a modeled execution error");
            }
            ++result.schedules_explored;
            return result;
        }
    }

    if (!any_enabled(program, state)) {
        ++result.schedules_explored;
        if (!all_finished(program, state)) {
            result.first_deadlock = make_deadlock_report(program, state);
        }
    }

    return result;
}

bool schedule_step_less(const ScheduleStep& lhs, const ScheduleStep& rhs) {
    if (lhs.thread != rhs.thread) {
        return lhs.thread < rhs.thread;
    }
    return lhs.action_index < rhs.action_index;
}

struct RaceIdentity {
    std::string address;
    ScheduleStep first;
    ScheduleStep second;

    bool operator==(const RaceIdentity&) const = default;
};

struct DeadlockIdentity {
    std::vector<BlockedThread> blocked_threads;

    bool operator==(const DeadlockIdentity&) const = default;
};

struct ErrorIdentity {
    ScheduleStep endpoint;

    bool operator==(const ErrorIdentity&) const = default;
};

struct BugIdentitySet {
    std::optional<RaceIdentity> race;
    std::optional<DeadlockIdentity> deadlock;
    std::optional<ErrorIdentity> error;
};

bool blocked_thread_less(const BlockedThread& lhs, const BlockedThread& rhs) {
    if (lhs.thread != rhs.thread) {
        return lhs.thread < rhs.thread;
    }
    if (lhs.kind != rhs.kind) {
        return lhs.kind < rhs.kind;
    }
    if (lhs.mutex != rhs.mutex) {
        return lhs.mutex < rhs.mutex;
    }
    if (lhs.owner.has_value() != rhs.owner.has_value()) {
        return !lhs.owner.has_value();
    }
    if (lhs.owner.value_or(0) != rhs.owner.value_or(0)) {
        return lhs.owner.value_or(0) < rhs.owner.value_or(0);
    }
    if (lhs.target.has_value() != rhs.target.has_value()) {
        return !lhs.target.has_value();
    }
    if (lhs.target.value_or(0) != rhs.target.value_or(0)) {
        return lhs.target.value_or(0) < rhs.target.value_or(0);
    }
    return lhs.condition < rhs.condition;
}

RaceIdentity identity_of(const RaceReport& report) {
    ScheduleStep first = report.first;
    ScheduleStep second = report.second;
    if (schedule_step_less(second, first)) {
        std::swap(first, second);
    }
    return RaceIdentity{report.address, first, second};
}

DeadlockIdentity identity_of(const DeadlockReport& report) {
    auto blocked = report.blocked_threads;
    std::sort(blocked.begin(), blocked.end(), blocked_thread_less);
    return DeadlockIdentity{std::move(blocked)};
}

ErrorIdentity identity_of(const ModelErrorReport& report) {
    return ErrorIdentity{report.endpoint};
}

BugIdentitySet identities_of(const CheckResult& result) {
    BugIdentitySet identities;
    if (result.first_race.has_value()) {
        identities.race = identity_of(*result.first_race);
    }
    if (result.first_deadlock.has_value()) {
        identities.deadlock = identity_of(*result.first_deadlock);
    }
    if (result.first_error.has_value()) {
        identities.error = identity_of(*result.first_error);
    }
    return identities;
}

bool empty(const BugIdentitySet& identities) {
    return !identities.race.has_value() &&
           !identities.deadlock.has_value() &&
           !identities.error.has_value();
}

bool reproduces_identities(const CheckResult& result, const BugIdentitySet& target) {
    if (target.race.has_value()) {
        if (!result.first_race.has_value() || identity_of(*result.first_race) != *target.race) {
            return false;
        }
    }
    if (target.deadlock.has_value()) {
        if (!result.first_deadlock.has_value() || identity_of(*result.first_deadlock) != *target.deadlock) {
            return false;
        }
    }
    if (target.error.has_value()) {
        if (!result.first_error.has_value() || identity_of(*result.first_error) != *target.error) {
            return false;
        }
    }
    return true;
}

bool is_report_endpoint(const ScheduleStep& step, const BugIdentitySet& target) {
    if (target.race.has_value() &&
        (step == target.race->first || step == target.race->second)) {
        return true;
    }
    if (target.error.has_value() && step == target.error->endpoint) {
        return true;
    }
    return false;
}

std::optional<std::size_t> last_step_index_for_thread(const Schedule& schedule, ThreadId tid) {
    for (std::size_t index = schedule.size(); index > 0; --index) {
        if (schedule[index - 1].thread == tid) {
            return index - 1;
        }
    }
    return std::nullopt;
}

Schedule minimize_schedule_for_identities(const Program& program,
                                          const Schedule& schedule,
                                          const BugIdentitySet& target) {
    Schedule minimized = schedule;
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t tid_index = 0; tid_index < program.threads.size(); ++tid_index) {
            const auto tid = static_cast<ThreadId>(tid_index);
            const auto last_step_index = last_step_index_for_thread(minimized, tid);
            if (!last_step_index.has_value()) {
                continue;
            }

            if (is_report_endpoint(minimized.at(*last_step_index), target)) {
                continue;
            }

            Schedule candidate = minimized;
            candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(*last_step_index));

            try {
                const CheckResult replayed = replay_schedule(program, candidate);
                if (reproduces_identities(replayed, target)) {
                    minimized = std::move(candidate);
                    changed = true;
                }
            } catch (const std::invalid_argument&) {
                // Removing a per-thread tail can still change enabledness for
                // later threads. Replay is the ground truth; invalid
                // candidates are rejected.
            }
        }
    }
    return minimized;
}

RaceReport minimized_race_report(const Program& program, const RaceReport& report) {
    BugIdentitySet target;
    target.race = identity_of(report);

    const Schedule minimized = minimize_schedule_for_identities(program, report.schedule, target);
    const CheckResult replayed = replay_schedule(program, minimized);
    if (!reproduces_identities(replayed, target) || !replayed.first_race.has_value()) {
        throw std::logic_error("race schedule minimization failed to preserve bug identity");
    }
    return *replayed.first_race;
}

DeadlockReport minimized_deadlock_report(const Program& program, const DeadlockReport& report) {
    BugIdentitySet target;
    target.deadlock = identity_of(report);

    const Schedule minimized = minimize_schedule_for_identities(program, report.schedule, target);
    const CheckResult replayed = replay_schedule(program, minimized);
    if (!reproduces_identities(replayed, target) || !replayed.first_deadlock.has_value()) {
        throw std::logic_error("deadlock schedule minimization failed to preserve bug identity");
    }
    return *replayed.first_deadlock;
}

ModelErrorReport minimized_error_report(const Program& program, const ModelErrorReport& report) {
    BugIdentitySet target;
    target.error = identity_of(report);

    const Schedule minimized = minimize_schedule_for_identities(program, report.schedule, target);
    const CheckResult replayed = replay_schedule(program, minimized);
    if (!reproduces_identities(replayed, target) || !replayed.first_error.has_value()) {
        throw std::logic_error("error schedule minimization failed to preserve bug identity");
    }
    return *replayed.first_error;
}

void minimize_result_reports(const Program& program, CheckResult& result) {
    if (result.first_race.has_value()) {
        result.first_race = minimized_race_report(program, *result.first_race);
    }
    if (result.first_deadlock.has_value()) {
        result.first_deadlock = minimized_deadlock_report(program, *result.first_deadlock);
    }
    if (result.first_error.has_value()) {
        result.first_error = minimized_error_report(program, *result.first_error);
    }
}

} // namespace

ModelChecker::ModelChecker(Program program) : program_(std::move(program)) {}

CheckResult ModelChecker::explore_naive(std::size_t max_schedules) const {
    CheckResult result;
    dfs(program_, initial_state(program_), result, max_schedules);
    minimize_result_reports(program_, result);
    return result;
}

CheckResult ModelChecker::explore_dpor(std::size_t max_schedules) const {
    CheckResult result;
    std::vector<DporNode> nodes;
    std::vector<ExecutedTransition> trace;
    dpor_dfs(program_, initial_state(program_), result, max_schedules, nodes, trace, {});
    minimize_result_reports(program_, result);
    return result;
}

CheckResult ModelChecker::replay(const Schedule& schedule) const {
    return replay_schedule(program_, schedule);
}

Schedule ModelChecker::minimize_schedule(const Schedule& schedule) const {
    const CheckResult replayed = replay_schedule(program_, schedule);
    const BugIdentitySet target = identities_of(replayed);
    if (empty(target)) {
        return schedule;
    }
    return minimize_schedule_for_identities(program_, schedule, target);
}

} // namespace model
