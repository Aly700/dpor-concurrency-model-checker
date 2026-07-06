#include "model/checker.hpp"

#include "model/vector_clock.hpp"

#include <algorithm>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace model {
namespace {

struct MemoryAccess {
    VectorClock clock;
    ScheduleStep endpoint;
};

struct AddressState {
    std::optional<MemoryAccess> last_write;
    std::map<std::pair<ThreadId, std::uint32_t>, MemoryAccess> reads_since_last_write;
};

struct ExecutionState {
    std::vector<std::uint32_t> pc;
    std::map<std::string, ThreadId> mutex_owner;
    std::map<std::string, VectorClock> mutex_clock;
    std::vector<VectorClock> thread_clock;
    std::map<std::string, AddressState> memory;
    Schedule schedule;
};

struct StepReport {
    std::optional<RaceReport> race;
    std::optional<ModelErrorReport> error;
};

struct DporNode {
    std::vector<ThreadId> enabled;
    std::vector<ThreadId> backtrack;
    std::vector<ThreadId> done;
};

struct ExecutedTransition {
    ThreadId thread{0};
    Action action;
    ScheduleStep endpoint;
};

ExecutionState initial_state(const Program& program) {
    return ExecutionState{
        std::vector<std::uint32_t>(program.threads.size(), 0),
        {},
        {},
        std::vector<VectorClock>(program.threads.size()),
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

bool is_enabled(const Program& program, const ExecutionState& state, ThreadId tid) {
    if (is_finished(program, state, tid)) {
        return false;
    }

    const Action& action = next_action(program, state, tid);
    if (action.kind != ActionKind::Lock) {
        return true;
    }

    return state.mutex_owner.find(action.mutex) == state.mutex_owner.end();
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
        if (is_enabled(program, state, tid)) {
            enabled.push_back(tid);
        }
    }
    return enabled;
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

    address_state.last_write = current;
    address_state.reads_since_last_write.clear();
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

StepReport execute_enabled_step(const Program& program, ExecutionState& state, ThreadId tid) {
    const auto action_index = state.pc.at(tid);
    const ScheduleStep endpoint{tid, action_index};
    const Action& action = program.threads.at(tid).at(action_index);

    ++state.pc.at(tid);
    state.schedule.push_back(endpoint);
    state.thread_clock.at(tid).tick(tid);

    StepReport report;
    switch (action.kind) {
    case ActionKind::Lock:
        state.mutex_owner[action.mutex] = tid;
        state.thread_clock.at(tid).join(state.mutex_clock[action.mutex]);
        break;
    case ActionKind::Unlock: {
        const auto owner = state.mutex_owner.find(action.mutex);
        if (owner == state.mutex_owner.end() || owner->second != tid) {
            report.error = make_unlock_error(action, endpoint, state.schedule, state);
            break;
        }
        state.mutex_clock[action.mutex] = state.thread_clock.at(tid);
        state.mutex_owner.erase(owner);
        break;
    }
    case ActionKind::Read:
        if (!action.address.empty()) {
            report.race = record_read(state, action, MemoryAccess{state.thread_clock.at(tid), endpoint});
        }
        break;
    case ActionKind::Write:
        if (!action.address.empty()) {
            report.race = record_write(state, action, MemoryAccess{state.thread_clock.at(tid), endpoint});
        }
        break;
    case ActionKind::Yield:
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
        if (action.kind == ActionKind::Lock) {
            const auto owner = state.mutex_owner.find(action.mutex);
            report.blocked_threads.push_back(BlockedThread{
                tid,
                action.mutex,
                owner == state.mutex_owner.end() ? std::optional<ThreadId>{} : std::optional<ThreadId>{owner->second},
            });
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

    insert_thread(node.backtrack, node.enabled.front());

    bool changed = true;
    while (changed) {
        changed = false;
        for (const ThreadId candidate : node.enabled) {
            if (contains_thread(node.backtrack, candidate)) {
                continue;
            }

            for (const ThreadId selected : node.backtrack) {
                if (!independent(next_action(program, state, candidate),
                                 next_action(program, state, selected))) {
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
    for (std::size_t previous_index = 0; previous_index < prefix_size; ++previous_index) {
        const ExecutedTransition& previous = trace.at(previous_index);
        if (previous.thread == current.thread) {
            // INVARIANTS.md Replay: schedules preserve per-thread action-index
            // order, so same-thread transitions cannot be swapped into a new
            // legal replay schedule.
            continue;
        }

        if (independent(previous.action, current.action)) {
            // INVARIANTS.md Soundness/Independence: this is the DPOR pruning
            // predicate. We may commute and therefore avoid a backtrack only
            // when independent() says ordering cannot affect observable state
            // or future enabledness.
            continue;
        }

        DporNode& backtrack_point = nodes.at(previous_index);
        if (contains_thread(backtrack_point.enabled, current.thread)) {
            // INVARIANTS.md Soundness: dependent transitions may expose a
            // distinct bug class, so force a representative that schedules the
            // later thread at the earlier dependent point when it was enabled.
            insert_thread(backtrack_point.backtrack, current.thread);
        } else {
            // INVARIANTS.md Soundness, highest-risk disabled-transition
            // fallback: the later dependent thread could not be scheduled at
            // this earlier point, so add every enabled thread. This is the
            // classic conservative DPOR fallback that preserves deadlock
            // detection by exploring whichever enabled transition may make the
            // dependent thread reachable before the earlier transition.
            for (const ThreadId enabled : backtrack_point.enabled) {
                insert_thread(backtrack_point.backtrack, enabled);
            }
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
            next_action(program, state, tid),
            ScheduleStep{tid, state.pc.at(tid)},
        };
        // INVARIANTS.md Soundness/Deadlock: a blocked transition absent from
        // the executed trace may be exactly the dependent action needed to
        // expose another deadlock, race, or error schedule. We therefore apply
        // the same independent()-guarded backtrack rule to disabled next
        // actions at terminal leaves; independent blocked actions remain
        // pruned only when the Independence invariant permits commuting them.
        add_backtracks_for_transition_against_prefix(nodes, trace, blocked, trace.size());
    }
}

void dpor_dfs(const Program& program,
              ExecutionState state,
              CheckResult& result,
              std::size_t max_schedules,
              std::vector<DporNode>& nodes,
              std::vector<ExecutedTransition>& trace) {
    if (result.schedules_explored >= max_schedules) {
        return;
    }

    const auto depth = nodes.size();
    nodes.push_back(DporNode{enabled_threads(program, state), {}, {}});

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

    while (result.schedules_explored < max_schedules) {
        const std::optional<ThreadId> next_tid = next_unexplored_backtrack(nodes.at(depth));
        if (!next_tid.has_value()) {
            break;
        }

        insert_thread(nodes.at(depth).done, *next_tid);
        if (!contains_thread(nodes.at(depth).enabled, *next_tid)) {
            continue;
        }

        const auto action_index = state.pc.at(*next_tid);
        const ExecutedTransition transition{
            *next_tid,
            program.threads.at(*next_tid).at(action_index),
            ScheduleStep{*next_tid, action_index},
        };

        ExecutionState next = state;
        const StepReport step_report = execute_enabled_step(program, next, *next_tid);
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
            dpor_dfs(program, std::move(next), result, max_schedules, nodes, trace);
        }

        trace.pop_back();
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

} // namespace

ModelChecker::ModelChecker(Program program) : program_(std::move(program)) {}

CheckResult ModelChecker::explore_naive(std::size_t max_schedules) const {
    CheckResult result;
    dfs(program_, initial_state(program_), result, max_schedules);
    return result;
}

CheckResult ModelChecker::explore_dpor(std::size_t max_schedules) const {
    CheckResult result;
    std::vector<DporNode> nodes;
    std::vector<ExecutedTransition> trace;
    dpor_dfs(program_, initial_state(program_), result, max_schedules, nodes, trace);
    return result;
}

CheckResult ModelChecker::replay(const Schedule& schedule) const {
    CheckResult result;
    ExecutionState state = initial_state(program_);

    for (std::size_t i = 0; i < schedule.size(); ++i) {
        validate_replay_step(program_, state, schedule[i], i);
        const StepReport step_report = execute_enabled_step(program_, state, schedule[i].thread);
        record_step_report(result, step_report);
        if (step_report.error.has_value()) {
            if (i + 1 < schedule.size()) {
                throw invalid_schedule(i + 1, "schedule continues after a modeled execution error");
            }
            ++result.schedules_explored;
            return result;
        }
    }

    if (!any_enabled(program_, state)) {
        ++result.schedules_explored;
        if (!all_finished(program_, state)) {
            result.first_deadlock = make_deadlock_report(program_, state);
        }
    }

    return result;
}

} // namespace model
