#include "model/checker.hpp"

#include "model/vector_clock.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
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
    std::vector<bool> started;
    std::map<std::string, ThreadId> mutex_owner;
    std::map<std::string, VectorClock> mutex_clock;
    std::map<std::string, std::vector<ThreadId>> condition_waiters;
    std::vector<VectorClock> thread_clock;
    std::vector<std::array<Value, kRegisterCount>> registers;
    std::vector<std::size_t> thread_steps;
    std::vector<WaitPhase> wait_phase;
    std::map<std::string, AddressState> memory;
    std::map<std::string, Value> memory_values;
    std::map<std::string, VectorClock> atomic_location_clock;
    Schedule schedule;
};

struct StepReport {
    std::optional<RaceReport> race;
    std::optional<ModelErrorReport> error;
    std::optional<AssertionFailureReport> assertion;
    std::optional<ThreadId> spawned_thread;
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
    std::vector<std::uint32_t> pc;
    std::vector<bool> started;
    std::map<std::string, ThreadId> mutex_owner;
    std::vector<WaitPhase> wait_phase;
};

struct ExecutedTransition {
    ThreadId thread{0};
    Action effective_action;
    ScheduleStep endpoint;
    VectorClock clock;
    std::optional<ThreadId> spawned_thread;
};

std::vector<bool> initially_started_threads(const Program& program) {
    std::vector<bool> started(program.threads.size(), true);
    for (ThreadId tid = 0; tid < program.threads.size(); ++tid) {
        for (const Action& action : program.threads.at(tid)) {
            if (action.kind == ActionKind::Spawn &&
                action.target < program.threads.size() &&
                action.target != tid) {
                started.at(action.target) = false;
            }
        }
    }
    return started;
}

std::vector<std::array<Value, kRegisterCount>> initial_registers(std::size_t thread_count) {
    std::vector<std::array<Value, kRegisterCount>> registers(thread_count);
    for (auto& thread_registers : registers) {
        thread_registers.fill(0);
    }
    return registers;
}

bool is_label_action(const Program& program, ThreadId tid, std::uint32_t pc) {
    return pc < program.threads.at(tid).size() &&
           program.threads.at(tid).at(pc).kind == ActionKind::Label;
}

void normalize_pc(const Program& program, ExecutionState& state, ThreadId tid) {
    while (is_label_action(program, tid, state.pc.at(tid))) {
        ++state.pc.at(tid);
    }
}

void normalize_all_pcs(const Program& program, ExecutionState& state) {
    for (ThreadId tid = 0; tid < program.threads.size(); ++tid) {
        normalize_pc(program, state, tid);
    }
}

ExecutionState initial_state(const Program& program) {
    ExecutionState state{
        std::vector<std::uint32_t>(program.threads.size(), 0),
        initially_started_threads(program),
        {},
        {},
        {},
        std::vector<VectorClock>(program.threads.size()),
        initial_registers(program.threads.size()),
        std::vector<std::size_t>(program.threads.size(), 0),
        std::vector<WaitPhase>(program.threads.size(), WaitPhase::None),
        {},
        {},
        {},
        {},
    };
    normalize_all_pcs(program, state);
    return state;
}

bool has_next_action(const Program& program, const ExecutionState& state, ThreadId tid) {
    return state.pc.at(tid) < program.threads.at(tid).size();
}

bool is_finished(const Program& program, const ExecutionState& state, ThreadId tid) {
    return state.started.at(tid) && !has_next_action(program, state, tid);
}

bool all_finished(const Program& program, const ExecutionState& state) {
    for (ThreadId tid = 0; tid < program.threads.size(); ++tid) {
        if (state.started.at(tid) && !is_finished(program, state, tid)) {
            return false;
        }
    }
    return true;
}

const Action& next_action(const Program& program, const ExecutionState& state, ThreadId tid) {
    const Action& action = program.threads.at(tid).at(state.pc.at(tid));
    assert(action.kind != ActionKind::Label);
    return action;
}

bool valid_register(RegisterId reg) {
    return reg < kRegisterCount;
}

Value read_register(const ExecutionState& state, ThreadId tid, RegisterId reg) {
    if (!valid_register(reg)) {
        throw std::logic_error("register id out of range");
    }
    return state.registers.at(tid).at(reg);
}

void write_register(ExecutionState& state, ThreadId tid, std::optional<RegisterId> reg, Value value) {
    if (!reg.has_value()) {
        return;
    }
    if (!valid_register(*reg)) {
        throw std::logic_error("register id out of range");
    }
    state.registers.at(tid).at(*reg) = value;
}

Value evaluate_operand(const ExecutionState& state, ThreadId tid, const ValueOperand& operand) {
    if (operand.kind == ValueOperandKind::Register) {
        return read_register(state, tid, operand.reg);
    }
    return operand.immediate;
}

Value evaluate_operand_or(const ExecutionState& state,
                          ThreadId tid,
                          const std::optional<ValueOperand>& operand,
                          Value default_value) {
    if (!operand.has_value()) {
        return default_value;
    }
    return evaluate_operand(state, tid, *operand);
}

std::optional<std::uint32_t> resolve_label(const Program& program, ThreadId tid, const std::string& label) {
    for (std::uint32_t index = 0; index < program.threads.at(tid).size(); ++index) {
        const Action& candidate = program.threads.at(tid).at(index);
        if (candidate.kind == ActionKind::Label && candidate.label == label) {
            return index;
        }
    }
    return std::nullopt;
}

void advance_pc(const Program& program, ExecutionState& state, ThreadId tid) {
    ++state.pc.at(tid);
    normalize_pc(program, state, tid);
}

void set_pc(const Program& program, ExecutionState& state, ThreadId tid, std::uint32_t pc) {
    state.pc.at(tid) = pc;
    normalize_pc(program, state, tid);
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

bool spawn_target_is_invalid(const Program& program, ThreadId tid, const Action& action) {
    return action.target >= program.threads.size() || action.target == tid;
}

bool is_enabled(const Program& program, const ExecutionState& state, ThreadId tid) {
    if (!state.started.at(tid)) {
        return false;
    }
    if (is_finished(program, state, tid)) {
        return false;
    }

    const Action& action = next_action(program, state, tid);
    switch (action.kind) {
    case ActionKind::Label:
        return false;
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
    case ActionKind::Set:
    case ActionKind::BranchNonzero:
    case ActionKind::Assert:
    case ActionKind::AtomicLoad:
    case ActionKind::AtomicStore:
    case ActionKind::AtomicRmw:
    case ActionKind::CompareExchange:
    case ActionKind::Spawn:
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

void remove_thread(std::vector<ThreadId>& threads, ThreadId tid) {
    const auto position = std::lower_bound(threads.begin(), threads.end(), tid);
    if (position != threads.end() && *position == tid) {
        threads.erase(position);
    }
}

bool transition_enabled_at_node(const DporNode& node, const ExecutedTransition& transition) {
    const auto enabled = node.enabled_transitions.find(transition.thread);
    return enabled != node.enabled_transitions.end() &&
           enabled->second.endpoint == transition.endpoint &&
           enabled->second.effective_action == transition.effective_action;
}

bool has_next_action_at_node(const Program& program, const DporNode& node, ThreadId tid) {
    return node.pc.at(tid) < program.threads.at(tid).size();
}

bool is_finished_at_node(const Program& program, const DporNode& node, ThreadId tid) {
    return node.started.at(tid) && !has_next_action_at_node(program, node, tid);
}

const Action& next_action_at_node(const Program& program, const DporNode& node, ThreadId tid) {
    const Action& action = program.threads.at(tid).at(node.pc.at(tid));
    assert(action.kind != ActionKind::Label);
    return action;
}

Action effective_next_action_at_node(const Program& program, const DporNode& node, ThreadId tid) {
    Action action = next_action_at_node(program, node, tid);
    if (action.kind == ActionKind::Wait && node.wait_phase.at(tid) == WaitPhase::Woken) {
        Action reacquire;
        reacquire.kind = ActionKind::Lock;
        reacquire.mutex = action.mutex;
        return reacquire;
    }
    return action;
}

bool enabled_at_node(const DporNode& node, ThreadId tid) {
    return contains_thread(node.enabled, tid);
}

std::vector<ThreadId> singleton_thread(ThreadId tid) {
    std::vector<ThreadId> threads;
    insert_thread(threads, tid);
    return threads;
}

void append_threads(std::vector<ThreadId>& destination, const std::vector<ThreadId>& source) {
    for (const ThreadId tid : source) {
        insert_thread(destination, tid);
    }
}

bool has_remaining_spawn_to(const Program& program,
                            const DporNode& node,
                            ThreadId source,
                            ThreadId target) {
    if (target >= program.threads.size() || source == target) {
        return false;
    }
    for (std::size_t index = node.pc.at(source); index < program.threads.at(source).size(); ++index) {
        const Action& action = program.threads.at(source).at(index);
        if (action.kind == ActionKind::Spawn && action.target == target) {
            return true;
        }
    }
    return false;
}

bool has_remaining_wake_on(const Program& program,
                           const DporNode& node,
                           ThreadId source,
                           const std::string& condition) {
    for (std::size_t index = node.pc.at(source); index < program.threads.at(source).size(); ++index) {
        const Action& action = program.threads.at(source).at(index);
        if ((action.kind == ActionKind::Signal || action.kind == ActionKind::Broadcast) &&
            action.condition == condition) {
            return true;
        }
    }
    return false;
}

std::optional<std::vector<ThreadId>> enabler_heads_for_thread(const Program& program,
                                                              const DporNode& node,
                                                              ThreadId tid,
                                                              std::vector<bool>& visiting);

std::optional<std::vector<ThreadId>> enabler_heads_for_spawn_target(const Program& program,
                                                                    const DporNode& node,
                                                                    ThreadId target,
                                                                    std::vector<bool>& visiting) {
    std::vector<ThreadId> heads;
    for (ThreadId source = 0; source < program.threads.size(); ++source) {
        if (!has_remaining_spawn_to(program, node, source, target)) {
            continue;
        }

        const auto source_heads = enabler_heads_for_thread(program, node, source, visiting);
        if (!source_heads.has_value()) {
            return std::nullopt;
        }
        append_threads(heads, *source_heads);
    }

    if (heads.empty()) {
        return std::nullopt;
    }
    return heads;
}

std::optional<std::vector<ThreadId>> enabler_heads_for_waiter(const Program& program,
                                                             const DporNode& node,
                                                             const Action& wait_action,
                                                             std::vector<bool>& visiting) {
    std::vector<ThreadId> heads;
    for (ThreadId source = 0; source < program.threads.size(); ++source) {
        if (!has_remaining_wake_on(program, node, source, wait_action.condition)) {
            continue;
        }

        const auto source_heads = enabler_heads_for_thread(program, node, source, visiting);
        if (!source_heads.has_value()) {
            return std::nullopt;
        }
        append_threads(heads, *source_heads);
    }

    if (heads.empty()) {
        return std::nullopt;
    }
    return heads;
}

std::optional<std::vector<ThreadId>> enabler_heads_for_mutex_owner(const Program& program,
                                                                   const DporNode& node,
                                                                   const std::string& mutex,
                                                                   ThreadId blocked_thread,
                                                                   std::vector<bool>& visiting) {
    const auto owner = node.mutex_owner.find(mutex);
    if (owner == node.mutex_owner.end() || owner->second == blocked_thread) {
        return std::nullopt;
    }
    return enabler_heads_for_thread(program, node, owner->second, visiting);
}

std::optional<std::vector<ThreadId>> enabler_heads_for_thread(const Program& program,
                                                              const DporNode& node,
                                                              ThreadId tid,
                                                              std::vector<bool>& visiting) {
    if (tid >= program.threads.size()) {
        return std::nullopt;
    }
    if (visiting.at(tid)) {
        return std::nullopt;
    }

    visiting.at(tid) = true;
    const auto clear_visiting = [&]() {
        visiting.at(tid) = false;
    };

    if (!node.started.at(tid)) {
        const auto heads = enabler_heads_for_spawn_target(program, node, tid, visiting);
        clear_visiting();
        return heads;
    }

    if (is_finished_at_node(program, node, tid)) {
        clear_visiting();
        return std::nullopt;
    }

    if (enabled_at_node(node, tid)) {
        clear_visiting();
        return singleton_thread(tid);
    }

    const Action static_action = next_action_at_node(program, node, tid);
    const Action effective_action = effective_next_action_at_node(program, node, tid);
    std::optional<std::vector<ThreadId>> heads;
    switch (effective_action.kind) {
    case ActionKind::Lock:
        heads = enabler_heads_for_mutex_owner(program, node, effective_action.mutex, tid, visiting);
        break;
    case ActionKind::Join:
        if (!join_target_is_invalid(program, tid, effective_action)) {
            heads = enabler_heads_for_thread(program, node, effective_action.target, visiting);
        }
        break;
    case ActionKind::Wait:
        if (node.wait_phase.at(tid) == WaitPhase::Waiting) {
            heads = enabler_heads_for_waiter(program, node, static_action, visiting);
        }
        break;
    default:
        break;
    }

    clear_visiting();
    return heads;
}

std::optional<std::vector<ThreadId>> disabled_repair_threads(const Program& program,
                                                             const DporNode& node,
                                                             const ExecutedTransition& current) {
    if (current.thread >= program.threads.size()) {
        return std::nullopt;
    }

    std::vector<bool> visiting(program.threads.size(), false);
    if (!node.started.at(current.thread)) {
        return enabler_heads_for_spawn_target(program, node, current.thread, visiting);
    }

    if (!has_next_action_at_node(program, node, current.thread)) {
        return std::nullopt;
    }

    if (node.pc.at(current.thread) < current.endpoint.action_index) {
        return enabler_heads_for_thread(program, node, current.thread, visiting);
    }

    if (node.pc.at(current.thread) != current.endpoint.action_index) {
        return std::nullopt;
    }

    const Action static_action = next_action_at_node(program, node, current.thread);
    const Action effective_action = effective_next_action_at_node(program, node, current.thread);
    switch (effective_action.kind) {
    case ActionKind::Join:
        if (static_action == current.effective_action &&
            !join_target_is_invalid(program, current.thread, static_action)) {
            return enabler_heads_for_thread(program, node, static_action.target, visiting);
        }
        break;
    case ActionKind::Wait:
        if (static_action == current.effective_action &&
            node.wait_phase.at(current.thread) == WaitPhase::Waiting) {
            return enabler_heads_for_waiter(program, node, static_action, visiting);
        }
        break;
    default:
        break;
    }

    return std::nullopt;
}

void add_repair_threads(DporNode& node, const std::vector<ThreadId>& threads) {
    for (const ThreadId tid : threads) {
        if (!contains_thread(node.enabled, tid)) {
            continue;
        }
        insert_thread(node.backtrack, tid);
        remove_thread(node.sleep, tid);
    }
}

void add_all_enabled_repair_threads(DporNode& node) {
    add_repair_threads(node, node.enabled);
}

bool join_independent_from_transition(const Program& program,
                                      ThreadId join_thread,
                                      const Action& join_action,
                                      ThreadId other_thread,
                                      const Action& other_action) {
    assert(join_action.kind == ActionKind::Join);
    if (join_target_is_invalid(program, join_thread, join_action)) {
        return false;
    }
    if (other_thread == join_action.target) {
        return false;
    }
    if (other_action.kind == ActionKind::Spawn) {
        return false;
    }
    if (other_action.kind == ActionKind::Join &&
        !join_target_is_invalid(program, other_thread, other_action) &&
        other_action.target == join_thread) {
        return false;
    }
    return true;
}

bool transitions_independent(const Program& program,
                             ThreadId lhs_thread,
                             const Action& lhs,
                             ThreadId rhs_thread,
                             const Action& rhs) {
    if (lhs_thread == rhs_thread) {
        return false;
    }

    if (lhs.kind == ActionKind::Join || rhs.kind == ActionKind::Join) {
        if (lhs.kind == ActionKind::Join &&
            !join_independent_from_transition(program, lhs_thread, lhs, rhs_thread, rhs)) {
            return false;
        }
        if (rhs.kind == ActionKind::Join &&
            !join_independent_from_transition(program, rhs_thread, rhs, lhs_thread, lhs)) {
            return false;
        }
        return true;
    }

    return independent(lhs, rhs);
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

ModelErrorReport make_spawn_error(const Program& program,
                                  const Action& action,
                                  ScheduleStep endpoint,
                                  const Schedule& schedule,
                                  const ExecutionState& state) {
    std::ostringstream message;
    message << "thread " << endpoint.thread << " attempted to spawn ";
    if (action.target == endpoint.thread) {
        message << "itself";
    } else if (action.target >= program.threads.size()) {
        message << "out-of-range thread " << action.target;
    } else {
        message << "already started thread " << action.target;
        std::size_t spawn_count = 0;
        for (ThreadId tid = 0; tid < program.threads.size(); ++tid) {
            for (const Action& candidate : program.threads.at(tid)) {
                if (candidate.kind == ActionKind::Spawn && candidate.target == action.target) {
                    ++spawn_count;
                }
            }
        }
        if (spawn_count > 1) {
            message << " (target has " << spawn_count << " Spawn actions)";
        }
    }
    (void)state;
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

ModelErrorReport make_branch_error(const Action& action, ScheduleStep endpoint, const Schedule& schedule) {
    std::ostringstream message;
    message << "thread " << endpoint.thread << " branched to unknown label '" << action.label << "'";
    return ModelErrorReport{endpoint, message.str(), schedule};
}

AssertionFailureReport make_assertion_failure(const Action& action,
                                              ScheduleStep endpoint,
                                              const Schedule& schedule,
                                              const ExecutionState& state) {
    const RegisterId reg = action.source_register.value_or(0);
    return AssertionFailureReport{endpoint, reg, read_register(state, endpoint.thread, reg), schedule};
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
    ++state.thread_steps.at(tid);
    state.thread_clock.at(tid).tick(tid);

    StepReport report;
    switch (action.kind) {
    case ActionKind::Label:
        assert(false && "label actions are normalized out before execution");
        break;
    case ActionKind::Set:
        write_register(state,
                       tid,
                       action.destination,
                       evaluate_operand_or(state, tid, action.value, 0));
        advance_pc(program, state, tid);
        break;
    case ActionKind::BranchNonzero: {
        const RegisterId reg = action.source_register.value_or(0);
        if (read_register(state, tid, reg) != 0) {
            const auto target = resolve_label(program, tid, action.label);
            if (!target.has_value()) {
                advance_pc(program, state, tid);
                report.error = make_branch_error(action, endpoint, state.schedule);
                break;
            }
            set_pc(program, state, tid, *target);
        } else {
            advance_pc(program, state, tid);
        }
        break;
    }
    case ActionKind::Assert:
        if (read_register(state, tid, action.source_register.value_or(0)) == 0) {
            advance_pc(program, state, tid);
            report.assertion = make_assertion_failure(action, endpoint, state.schedule, state);
            break;
        }
        advance_pc(program, state, tid);
        break;
    case ActionKind::Lock:
        advance_pc(program, state, tid);
        state.mutex_owner[action.mutex] = tid;
        state.thread_clock.at(tid).join(state.mutex_clock[action.mutex]);
        break;
    case ActionKind::Join:
        advance_pc(program, state, tid);
        if (join_target_is_invalid(program, tid, action)) {
            report.error = make_join_error(action, endpoint, state.schedule);
            break;
        }
        state.thread_clock.at(tid).join(state.thread_clock.at(action.target));
        break;
    case ActionKind::Spawn:
        advance_pc(program, state, tid);
        if (spawn_target_is_invalid(program, tid, action) || state.started.at(action.target)) {
            report.error = make_spawn_error(program, action, endpoint, state.schedule, state);
            break;
        }
        state.started.at(action.target) = true;
        normalize_pc(program, state, action.target);
        state.thread_clock.at(action.target).join(state.thread_clock.at(tid));
        report.spawned_thread = action.target;
        break;
    case ActionKind::Unlock: {
        advance_pc(program, state, tid);
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
            advance_pc(program, state, tid);
            break;
        }

        assert(state.wait_phase.at(tid) == WaitPhase::None);
        if (!owns_mutex(state, tid, action.mutex)) {
            advance_pc(program, state, tid);
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
        advance_pc(program, state, tid);
        signal_one_waiter(state, action, tid);
        break;
    case ActionKind::Broadcast:
        advance_pc(program, state, tid);
        broadcast_waiters(state, action, tid);
        break;
    case ActionKind::AtomicLoad:
        advance_pc(program, state, tid);
        if (!action.address.empty()) {
            // Acquire load: join the thread with exactly this location's last
            // release sequence clock. Missing this edge over-reports races
            // after a real synchronizes-with relation; adding any extra edge
            // can hide a real plain-data race, so the model only joins the
            // per-address atomic clock and does not mutate it.
            const Value loaded = state.memory_values[action.address];
            state.thread_clock.at(tid).join(state.atomic_location_clock[action.address]);
            write_register(state, tid, action.destination, loaded);
            report.race = record_atomic(
                state,
                action,
                MemoryAccess{state.thread_clock.at(tid), endpoint, true, false});
        }
        break;
    case ActionKind::AtomicStore:
        advance_pc(program, state, tid);
        if (!action.address.empty()) {
            // Release store: replace the location clock with the storing
            // thread's post-tick clock. It must not join the previous
            // location clock; such an extra HB edge would fabricate ordering
            // from an earlier other-thread store to a later load and could
            // suppress a real plain-data race. The store itself performs no
            // acquire join, intentionally erring toward fewer edges.
            state.memory_values[action.address] = evaluate_operand_or(state, tid, action.value, 0);
            state.atomic_location_clock[action.address] = state.thread_clock.at(tid);
            report.race = record_atomic(
                state,
                action,
                MemoryAccess{state.thread_clock.at(tid), endpoint, true, true});
        }
        break;
    case ActionKind::AtomicRmw:
        advance_pc(program, state, tid);
        if (!action.address.empty()) {
            // Acq_rel RMW: first acquire from the current location clock, then
            // replace the location clock with the joined thread clock. The
            // acquire half preserves the C++ release-sequence edge; the
            // replace half avoids accumulating unrelated previous stores. When
            // forced to choose, the model avoids extra HB edges because they
            // hide races, while missing edges only over-report.
            const Value old_value = state.memory_values[action.address];
            const Value addend = evaluate_operand_or(state, tid, action.value, 1);
            state.thread_clock.at(tid).join(state.atomic_location_clock[action.address]);
            write_register(state, tid, action.destination, old_value);
            state.memory_values[action.address] = old_value + addend;
            state.atomic_location_clock[action.address] = state.thread_clock.at(tid);
            report.race = record_atomic(
                state,
                action,
                MemoryAccess{state.thread_clock.at(tid), endpoint, true, true});
        }
        break;
    case ActionKind::CompareExchange:
        advance_pc(program, state, tid);
        if (!action.address.empty()) {
            const Value old_value = state.memory_values[action.address];
            const Value expected = evaluate_operand_or(state, tid, action.expected, 0);
            const Value desired = evaluate_operand_or(state, tid, action.value, 0);
            const bool success = old_value == expected;
            // CAS failure is acquire-only: it joins from the current location
            // clock but must not replace it. Replacing on failure would publish
            // this thread's prior plain accesses and can hide real races.
            state.thread_clock.at(tid).join(state.atomic_location_clock[action.address]);
            write_register(state, tid, action.destination, success ? 1 : 0);
            if (success) {
                state.memory_values[action.address] = desired;
                state.atomic_location_clock[action.address] = state.thread_clock.at(tid);
            }
            report.race = record_atomic(
                state,
                action,
                MemoryAccess{state.thread_clock.at(tid), endpoint, true, success});
        }
        break;
    case ActionKind::Read:
        advance_pc(program, state, tid);
        if (!action.address.empty()) {
            write_register(state, tid, action.destination, state.memory_values[action.address]);
            report.race = record_read(
                state,
                action,
                MemoryAccess{state.thread_clock.at(tid), endpoint, false, false});
        }
        break;
    case ActionKind::Write:
        advance_pc(program, state, tid);
        if (!action.address.empty()) {
            state.memory_values[action.address] = evaluate_operand_or(state, tid, action.value, 0);
            report.race = record_write(
                state,
                action,
                MemoryAccess{state.thread_clock.at(tid), endpoint, false, true});
        }
        break;
    case ActionKind::Yield:
        advance_pc(program, state, tid);
        break;
    }
    return report;
}

DeadlockReport make_deadlock_report(const Program& program, const ExecutionState& state) {
    DeadlockReport report;
    for (ThreadId tid = 0; tid < program.threads.size(); ++tid) {
        if (!state.started.at(tid) || is_finished(program, state, tid)) {
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
    if (report.assertion.has_value() && !result.first_assertion.has_value()) {
        result.first_assertion = report.assertion;
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
                if (!transitions_independent(program,
                                             candidate,
                                             effective_next_action(program, state, candidate),
                                             selected,
                                             effective_next_action(program, state, selected))) {
                    // INVARIANTS.md Soundness/Independence: an enabled
                    // transition is pruned from the initial persistent set
                    // only when the transition predicate says it commutes
                    // with every selected enabled transition. A dependent
                    // enabled transition is kept so a distinct bug class is
                    // not skipped.
                    insert_thread(node.backtrack, candidate);
                    changed = true;
                    break;
                }
            }
        }
    }
}

void add_backtracks_for_transition_against_prefix(const Program& program,
                                                  std::vector<DporNode>& nodes,
                                                  const std::vector<ExecutedTransition>& trace,
                                                  const ExecutedTransition& current,
                                                  std::size_t prefix_size) {
    std::vector<std::size_t> disabled_dependent_prefixes;
    std::optional<std::size_t> spawn_enabler_index;
    for (std::size_t index = 0; index < prefix_size; ++index) {
        const ExecutedTransition& previous = trace.at(index);
        if (previous.spawned_thread.has_value() &&
            *previous.spawned_thread == current.thread) {
            spawn_enabler_index = index;
        }
    }

    for (std::size_t index = prefix_size; index > 0; --index) {
        const std::size_t previous_index = index - 1;
        const ExecutedTransition& previous = trace.at(previous_index);
        if (previous.thread == current.thread) {
            // INVARIANTS.md Replay: schedules preserve per-thread action-index
            // order, so same-thread transitions cannot be swapped into a new
            // legal replay schedule.
            continue;
        }

        if (transitions_independent(program,
                                    previous.thread,
                                    previous.effective_action,
                                    current.thread,
                                    current.effective_action)) {
            // INVARIANTS.md Soundness/Independence: this is the DPOR pruning
            // predicate. We may commute and therefore avoid a backtrack only
            // when the transition predicate says ordering cannot affect
            // observable state or future enabledness.
            continue;
        }

        DporNode& backtrack_point = nodes.at(previous_index);
        if (!transition_enabled_at_node(backtrack_point, current)) {
            if (spawn_enabler_index.has_value() && previous_index <= *spawn_enabler_index) {
                // Spawn(t) is the enabler for all later transitions in t. If
                // t's transition is disabled at or before that spawn prefix,
                // repairing there adds no useful alternative because t is not
                // started yet. Keep the disabled repair after the spawn,
                // where t's first action can run before the dependent
                // transition that blocked this later action.
                continue;
            }
            // Disabled-transition fallback is about reaching the later
            // effective transition at all, not just reversing two already
            // enabled transitions. A thread may be enabled here with an
            // earlier action, or a Wait may be enabled in the opposite phase;
            // in both cases an HB edge observed in this trace can disappear.
            // Add conservative repairs for every dependent disabled prefix:
            // earlier conservative dependencies such as Spawn or Join may be
            // real but still too early to make the later action reachable,
            // while the later prefix is the one that lets the action's own
            // prerequisites run before the dependent transition.
            disabled_dependent_prefixes.push_back(previous_index);
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
        remove_thread(backtrack_point.sleep, current.thread);
        return;
    }

    for (const std::size_t disabled_prefix : disabled_dependent_prefixes) {
        DporNode& backtrack_point = nodes.at(disabled_prefix);
        if (const auto repair_threads = disabled_repair_threads(program, backtrack_point, current)) {
            // INVARIANTS.md Soundness/Enabledness: when the disabled
            // transition's concrete enabler chain is known at this prefix,
            // only the first enabled heads of that chain can make the later
            // transition reachable before the dependent earlier transition.
            // Omitted enabled threads do not start the missing thread, finish
            // the join target, or wake the sleeping waiter; independent bug
            // classes involving them remain covered by normal DPOR repairs.
            add_repair_threads(backtrack_point, *repair_threads);
        } else {
            // Conservative fallback for blocked locks, woken reacquires, and
            // any chain we cannot compute. The later effective transition
            // could not be scheduled here, so add every enabled thread just as
            // ADR 0010 required.
            add_all_enabled_repair_threads(backtrack_point);
        }
    }
}

void add_backtracks_for_transition(const Program& program,
                                   std::vector<DporNode>& nodes,
                                   const std::vector<ExecutedTransition>& trace) {
    if (trace.empty()) {
        return;
    }

    add_backtracks_for_transition_against_prefix(program, nodes, trace, trace.back(), trace.size() - 1);
}

void add_disabled_backtracks(const Program& program,
                             const ExecutionState& state,
                             std::vector<DporNode>& nodes,
                             const std::vector<ExecutedTransition>& trace) {
    for (ThreadId tid = 0; tid < program.threads.size(); ++tid) {
        if (is_finished(program, state, tid) || is_enabled(program, state, tid)) {
            continue;
        }
        if (!has_next_action(program, state, tid)) {
            continue;
        }

        const ExecutedTransition blocked{
            tid,
            effective_next_action(program, state, tid),
            ScheduleStep{tid, state.pc.at(tid)},
            state.thread_clock.at(tid),
            std::nullopt,
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
        add_backtracks_for_transition_against_prefix(program, nodes, trace, blocked, trace.size());
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
        if (transitions_independent(program,
                                    tid,
                                    slept_action,
                                    transition.thread,
                                    transition.effective_action)) {
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

bool step_bound_reached(const ExecutionState& state, ThreadId tid, std::size_t step_bound) {
    return state.thread_steps.at(tid) >= step_bound;
}

void record_bound_exceeded(CheckResult& result) {
    ++result.schedules_explored;
    ++result.bound_exceeded_executions;
}

void dpor_dfs(const Program& program,
              ExecutionState state,
              CheckResult& result,
              std::size_t max_schedules,
              std::size_t step_bound,
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
        state.pc,
        state.started,
        state.mutex_owner,
        state.wait_phase,
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
            // Join, not-started Spawn targets, and Wait-reacquire transitions
            // can still be the evidence that an earlier enabledness repair is
            // needed, especially before a slept modeled-error endpoint. Apply
            // the terminal disabled fallback before pruning the slept
            // representative.
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

        if (step_bound_reached(state, *next_tid, step_bound)) {
            nodes.at(depth).sleep.clear();
            for (const ThreadId enabled : nodes.at(depth).enabled) {
                insert_thread(nodes.at(depth).backtrack, enabled);
            }
            record_bound_exceeded(result);
            insert_thread(nodes.at(depth).sleep, *next_tid);
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
            step_report.spawned_thread,
        };
        trace.push_back(transition);
        add_backtracks_for_transition(program, nodes, trace);
        record_step_report(result, step_report);

        if (step_report.error.has_value() || step_report.assertion.has_value()) {
            add_disabled_backtracks(program, state, nodes, trace);
            // INVARIANTS.md Soundness: a modeled error terminates this
            // schedule, and assertion failure does the same. Even independent
            // enabled siblings cannot be represented by running them after the
            // terminal endpoint. Do not prune at this node after such an
            // endpoint; add every enabled sibling so races, deadlocks, other
            // errors, or assertions reachable before it still have a
            // representative schedule.
            nodes.at(depth).sleep.clear();
            for (const ThreadId enabled : nodes.at(depth).enabled) {
                insert_thread(nodes.at(depth).backtrack, enabled);
            }
            ++result.schedules_explored;
        } else {
            // INVARIANTS.md Soundness/Independence: enabled transitions not in
            // this node's backtrack set are the only schedules pruned here.
            // The initial persistent set and dynamic backtrack additions above
            // omit an alternative solely after the transition predicate
            // justifies commuting it with the representative transition.
            std::vector<ThreadId> child_sleep =
                inherited_sleep_set(program, next, nodes.at(depth), transition);
            dpor_dfs(program,
                     std::move(next),
                     result,
                     max_schedules,
                     step_bound,
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
         std::size_t max_schedules,
         std::size_t step_bound) {
    if (result.schedules_explored >= max_schedules) {
        return;
    }

    bool explored_child = false;
    for (ThreadId tid = 0; tid < program.threads.size(); ++tid) {
        if (!is_enabled(program, state, tid)) {
            continue;
        }

        explored_child = true;
        if (step_bound_reached(state, tid, step_bound)) {
            record_bound_exceeded(result);
            if (result.schedules_explored >= max_schedules) {
                return;
            }
            continue;
        }

        ExecutionState next = state;
        const StepReport step_report = execute_enabled_step(program, next, tid);
        record_step_report(result, step_report);
        if (step_report.error.has_value() || step_report.assertion.has_value()) {
            ++result.schedules_explored;
        } else {
            dfs(program, std::move(next), result, max_schedules, step_bound);
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

CheckResult replay_schedule(const Program& program, const Schedule& schedule, std::size_t step_bound) {
    CheckResult result;
    ExecutionState state = initial_state(program);

    for (std::size_t i = 0; i < schedule.size(); ++i) {
        validate_replay_step(program, state, schedule[i], i);
        if (step_bound_reached(state, schedule[i].thread, step_bound)) {
            if (i + 1 < schedule.size()) {
                throw invalid_schedule(i + 1, "schedule continues after a step-bound outcome");
            }
            record_bound_exceeded(result);
            return result;
        }
        const StepReport step_report = execute_enabled_step(program, state, schedule[i].thread);
        record_step_report(result, step_report);
        if (step_report.error.has_value() || step_report.assertion.has_value()) {
            if (i + 1 < schedule.size()) {
                throw invalid_schedule(i + 1, "schedule continues after a terminal execution report");
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

struct AssertionIdentity {
    ScheduleStep endpoint;
    RegisterId reg{0};
    Value value{0};

    bool operator==(const AssertionIdentity&) const = default;
};

struct BugIdentitySet {
    std::optional<RaceIdentity> race;
    std::optional<DeadlockIdentity> deadlock;
    std::optional<ErrorIdentity> error;
    std::optional<AssertionIdentity> assertion;
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

AssertionIdentity identity_of(const AssertionFailureReport& report) {
    return AssertionIdentity{report.endpoint, report.reg, report.value};
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
    if (result.first_assertion.has_value()) {
        identities.assertion = identity_of(*result.first_assertion);
    }
    return identities;
}

bool empty(const BugIdentitySet& identities) {
    return !identities.race.has_value() &&
           !identities.deadlock.has_value() &&
           !identities.error.has_value() &&
           !identities.assertion.has_value();
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
    if (target.assertion.has_value()) {
        if (!result.first_assertion.has_value() ||
            identity_of(*result.first_assertion) != *target.assertion) {
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
    if (target.assertion.has_value() && step == target.assertion->endpoint) {
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
                                          const BugIdentitySet& target,
                                          std::size_t step_bound) {
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
                const CheckResult replayed = replay_schedule(program, candidate, step_bound);
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

RaceReport minimized_race_report(const Program& program,
                                 const RaceReport& report,
                                 std::size_t step_bound) {
    BugIdentitySet target;
    target.race = identity_of(report);

    const Schedule minimized = minimize_schedule_for_identities(program, report.schedule, target, step_bound);
    const CheckResult replayed = replay_schedule(program, minimized, step_bound);
    if (!reproduces_identities(replayed, target) || !replayed.first_race.has_value()) {
        throw std::logic_error("race schedule minimization failed to preserve bug identity");
    }
    return *replayed.first_race;
}

DeadlockReport minimized_deadlock_report(const Program& program,
                                         const DeadlockReport& report,
                                         std::size_t step_bound) {
    BugIdentitySet target;
    target.deadlock = identity_of(report);

    const Schedule minimized = minimize_schedule_for_identities(program, report.schedule, target, step_bound);
    const CheckResult replayed = replay_schedule(program, minimized, step_bound);
    if (!reproduces_identities(replayed, target) || !replayed.first_deadlock.has_value()) {
        throw std::logic_error("deadlock schedule minimization failed to preserve bug identity");
    }
    return *replayed.first_deadlock;
}

ModelErrorReport minimized_error_report(const Program& program,
                                        const ModelErrorReport& report,
                                        std::size_t step_bound) {
    BugIdentitySet target;
    target.error = identity_of(report);

    const Schedule minimized = minimize_schedule_for_identities(program, report.schedule, target, step_bound);
    const CheckResult replayed = replay_schedule(program, minimized, step_bound);
    if (!reproduces_identities(replayed, target) || !replayed.first_error.has_value()) {
        throw std::logic_error("error schedule minimization failed to preserve bug identity");
    }
    return *replayed.first_error;
}

AssertionFailureReport minimized_assertion_report(const Program& program,
                                                  const AssertionFailureReport& report,
                                                  std::size_t step_bound) {
    BugIdentitySet target;
    target.assertion = identity_of(report);

    const Schedule minimized = minimize_schedule_for_identities(program, report.schedule, target, step_bound);
    const CheckResult replayed = replay_schedule(program, minimized, step_bound);
    if (!reproduces_identities(replayed, target) || !replayed.first_assertion.has_value()) {
        throw std::logic_error("assertion schedule minimization failed to preserve bug identity");
    }
    return *replayed.first_assertion;
}

void minimize_result_reports(const Program& program, CheckResult& result, std::size_t step_bound) {
    if (result.first_race.has_value()) {
        result.first_race = minimized_race_report(program, *result.first_race, step_bound);
    }
    if (result.first_deadlock.has_value()) {
        result.first_deadlock = minimized_deadlock_report(program, *result.first_deadlock, step_bound);
    }
    if (result.first_error.has_value()) {
        result.first_error = minimized_error_report(program, *result.first_error, step_bound);
    }
    if (result.first_assertion.has_value()) {
        result.first_assertion = minimized_assertion_report(program, *result.first_assertion, step_bound);
    }
}

} // namespace

ModelChecker::ModelChecker(Program program, std::size_t step_bound)
    : program_(std::move(program)), step_bound_(step_bound) {
    if (step_bound_ == 0) {
        throw std::invalid_argument("step bound must be greater than zero");
    }
}

CheckResult ModelChecker::explore_naive(std::size_t max_schedules) const {
    CheckResult result;
    dfs(program_, initial_state(program_), result, max_schedules, step_bound_);
    result.exploration_capped = result.schedules_explored >= max_schedules;
    minimize_result_reports(program_, result, step_bound_);
    return result;
}

CheckResult ModelChecker::explore_dpor(std::size_t max_schedules) const {
    CheckResult result;
    std::vector<DporNode> nodes;
    std::vector<ExecutedTransition> trace;
    dpor_dfs(program_, initial_state(program_), result, max_schedules, step_bound_, nodes, trace, {});
    result.exploration_capped = result.schedules_explored >= max_schedules;
    minimize_result_reports(program_, result, step_bound_);
    return result;
}

CheckResult ModelChecker::replay(const Schedule& schedule) const {
    return replay_schedule(program_, schedule, step_bound_);
}

Schedule ModelChecker::minimize_schedule(const Schedule& schedule) const {
    const CheckResult replayed = replay_schedule(program_, schedule, step_bound_);
    const BugIdentitySet target = identities_of(replayed);
    if (empty(target)) {
        return schedule;
    }
    return minimize_schedule_for_identities(program_, schedule, target, step_bound_);
}

} // namespace model
