#include "model/checker.hpp"

#include "model/vector_clock.hpp"

#if defined(DPOR_EXPLORATION_METRICS)
#include "model/exploration_metrics.hpp"
#endif

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <set>
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

    bool operator==(const MemoryAccess&) const = default;
};

struct AddressState {
    std::optional<MemoryAccess> last_write;
    std::map<std::pair<ThreadId, std::uint32_t>, MemoryAccess> reads_since_last_write;
    std::vector<MemoryAccess> plain_accesses;
    std::vector<MemoryAccess> atomic_accesses;

    bool operator==(const AddressState&) const = default;
};

struct StoreBufferEntry {
    std::string address;
    Value value{0};

    bool operator==(const StoreBufferEntry&) const = default;
};

using PsoAddressBuffer = std::map<std::string, std::deque<Value>>;

enum class WaitPhase { None, Waiting, Woken };

struct RwLockState {
    // std::set provides one canonical holder representation and deterministic
    // iteration for fingerprints, deadlock reports, and enabler-chain repair.
    std::set<ThreadId> reader_holders;
    std::optional<ThreadId> writer_holder;
    VectorClock writer_release;
    VectorClock reader_releases;

    bool operator==(const RwLockState&) const = default;
};

struct SemaphoreState {
    std::size_t permits{0};
    VectorClock post_releases;

    bool operator==(const SemaphoreState&) const = default;
};

struct BarrierState {
    // The ordinal is private occurrence identity for DPOR/replay bookkeeping.
    // It is deliberately excluded from behavioral fingerprints because source
    // programs cannot observe it.
    std::uint64_t generation{0};
    std::uint32_t parties{0};
    std::set<ThreadId> arrivals;
    VectorClock arrival_releases;

    bool operator==(const BarrierState&) const = default;
};

struct ExecutionState {
    MemoryModel memory_model{MemoryModel::SC};
    std::vector<std::uint32_t> pc;
    std::vector<bool> started;
    std::map<std::string, ThreadId> mutex_owner;
    std::map<std::string, VectorClock> mutex_clock;
    std::map<std::string, RwLockState> rwlocks;
    std::map<std::string, SemaphoreState> semaphores;
    std::map<std::string, BarrierState> barriers;
    std::map<std::string, std::vector<ThreadId>> condition_waiters;
    std::vector<VectorClock> thread_clock;
    std::vector<std::array<Value, kRegisterCount>> registers;
    std::vector<std::size_t> thread_steps;
    std::vector<WaitPhase> wait_phase;
    std::map<std::string, AddressState> memory;
    std::map<std::string, Value> memory_values;
    std::map<std::string, VectorClock> atomic_location_clock;
    // TSO remains a single per-thread FIFO. PSO uses a separate per-address
    // FIFO map so changing PSO cannot perturb the established TSO path.
    std::vector<std::deque<StoreBufferEntry>> store_buffers;
    std::vector<PsoAddressBuffer> pso_store_buffers;
    std::vector<std::string> flush_addresses;
    Schedule schedule;

    bool operator==(const ExecutionState&) const = default;
};

#if defined(DPOR_EXPLORATION_METRICS)
diagnostics::ExplorationMetrics& profile_metrics() {
    return diagnostics::detail::mutable_exploration_metrics();
}

std::uint64_t diagnostic_clock_components(const VectorClock& clock) {
    return clock.diagnostic_component_count();
}

std::uint64_t diagnostic_action_string_bytes(const Action& action) {
    return action.address.size() + action.mutex.size() + action.rwlock.size() +
           action.semaphore.size() + action.condition.size() + action.label.size() +
           action.barrier.size();
}

void record_branch_state_copy(const ExecutionState& state, bool collector = false) {
    auto& metrics = profile_metrics();
    ++metrics.branch_state_copies;
    if (collector) {
        ++metrics.collector_state_copies;
    }

    metrics.branch_state_schedule_steps_copied += state.schedule.size();
    metrics.branch_state_container_elements_copied +=
        state.pc.size() + state.started.size() + state.thread_clock.size() +
        state.registers.size() + state.thread_steps.size() + state.wait_phase.size() +
        state.store_buffers.size() + state.pso_store_buffers.size() +
        state.flush_addresses.size();
    metrics.branch_state_map_nodes_copied +=
        state.mutex_owner.size() + state.mutex_clock.size() + state.rwlocks.size() +
        state.semaphores.size() + state.barriers.size() +
        state.condition_waiters.size() + state.memory.size() +
        state.memory_values.size() + state.atomic_location_clock.size();

    for (const auto& [name, owner] : state.mutex_owner) {
        metrics.branch_state_string_bytes_copied += name.size();
        (void)owner;
    }
    for (const auto& [name, clock] : state.mutex_clock) {
        metrics.branch_state_string_bytes_copied += name.size();
        metrics.branch_state_clock_components_copied += diagnostic_clock_components(clock);
    }
    for (const auto& [name, rwlock] : state.rwlocks) {
        metrics.branch_state_string_bytes_copied += name.size();
        metrics.branch_state_map_nodes_copied += rwlock.reader_holders.size();
        metrics.branch_state_container_elements_copied += rwlock.reader_holders.size();
        metrics.branch_state_clock_components_copied +=
            diagnostic_clock_components(rwlock.writer_release) +
            diagnostic_clock_components(rwlock.reader_releases);
    }
    for (const auto& [name, semaphore] : state.semaphores) {
        metrics.branch_state_string_bytes_copied += name.size();
        metrics.branch_state_clock_components_copied +=
            diagnostic_clock_components(semaphore.post_releases);
    }
    for (const auto& [name, barrier] : state.barriers) {
        metrics.branch_state_string_bytes_copied += name.size();
        metrics.branch_state_map_nodes_copied += barrier.arrivals.size();
        metrics.branch_state_container_elements_copied += barrier.arrivals.size();
        metrics.branch_state_clock_components_copied +=
            diagnostic_clock_components(barrier.arrival_releases);
    }
    for (const auto& [condition, waiters] : state.condition_waiters) {
        metrics.branch_state_string_bytes_copied += condition.size();
        metrics.branch_state_container_elements_copied += waiters.size();
    }
    for (const VectorClock& clock : state.thread_clock) {
        metrics.branch_state_clock_components_copied += diagnostic_clock_components(clock);
    }
    for (const auto& [address, address_state] : state.memory) {
        metrics.branch_state_string_bytes_copied += address.size();
        metrics.branch_state_map_nodes_copied += address_state.reads_since_last_write.size();
        const std::uint64_t access_records =
            (address_state.last_write.has_value() ? 1U : 0U) +
            address_state.reads_since_last_write.size() +
            address_state.plain_accesses.size() + address_state.atomic_accesses.size();
        metrics.branch_state_access_records_copied += access_records;
        metrics.branch_state_container_elements_copied +=
            address_state.plain_accesses.size() + address_state.atomic_accesses.size();
        const auto add_access_clock = [&](const MemoryAccess& access) {
            metrics.branch_state_clock_components_copied +=
                diagnostic_clock_components(access.clock);
        };
        if (address_state.last_write.has_value()) {
            add_access_clock(*address_state.last_write);
        }
        for (const auto& [endpoint, access] : address_state.reads_since_last_write) {
            add_access_clock(access);
            (void)endpoint;
        }
        for (const MemoryAccess& access : address_state.plain_accesses) {
            add_access_clock(access);
        }
        for (const MemoryAccess& access : address_state.atomic_accesses) {
            add_access_clock(access);
        }
    }
    for (const auto& [address, value] : state.memory_values) {
        metrics.branch_state_string_bytes_copied += address.size();
        (void)value;
    }
    for (const auto& [address, clock] : state.atomic_location_clock) {
        metrics.branch_state_string_bytes_copied += address.size();
        metrics.branch_state_clock_components_copied += diagnostic_clock_components(clock);
    }
    for (const auto& buffer : state.store_buffers) {
        metrics.branch_state_buffer_entries_copied += buffer.size();
        metrics.branch_state_container_elements_copied += buffer.size();
        for (const StoreBufferEntry& entry : buffer) {
            metrics.branch_state_string_bytes_copied += entry.address.size();
        }
    }
    for (const PsoAddressBuffer& buffers : state.pso_store_buffers) {
        metrics.branch_state_map_nodes_copied += buffers.size();
        for (const auto& [address, values] : buffers) {
            metrics.branch_state_string_bytes_copied += address.size();
            metrics.branch_state_buffer_entries_copied += values.size();
            metrics.branch_state_container_elements_copied += values.size();
        }
    }
    for (const std::string& address : state.flush_addresses) {
        metrics.branch_state_string_bytes_copied += address.size();
    }
}

void record_report_schedule_copy(const Schedule& schedule) {
    auto& metrics = profile_metrics();
    ++metrics.report_schedule_copies;
    metrics.report_schedule_steps_copied += schedule.size();
}
#endif

using StateFingerprint = std::string;
using StateHistory = std::map<StateFingerprint, std::size_t>;

void append_u64(StateFingerprint& fingerprint, std::uint64_t value) {
    // A fixed little-endian encoding avoids object-layout, padding, locale,
    // and host-endianness dependencies. Equality compares every byte; this is
    // deliberately not a hash because a collision could fabricate a cycle.
    for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
        fingerprint.push_back(static_cast<char>((value >> (byte * 8U)) & 0xffU));
    }
}

void append_string(StateFingerprint& fingerprint, const std::string& value) {
    append_u64(fingerprint, static_cast<std::uint64_t>(value.size()));
    fingerprint.append(value);
}

StateFingerprint behavioral_state_fingerprint(const ExecutionState& state) {
    StateFingerprint fingerprint;
    append_string(fingerprint, "dpor-behavioral-state-v1");
    append_u64(fingerprint, static_cast<std::uint64_t>(state.memory_model));

    append_u64(fingerprint, static_cast<std::uint64_t>(state.pc.size()));
    for (const std::uint32_t pc : state.pc) {
        append_u64(fingerprint, pc);
    }

    append_u64(fingerprint, static_cast<std::uint64_t>(state.started.size()));
    for (const bool started : state.started) {
        append_u64(fingerprint, started ? 1U : 0U);
    }

    append_u64(fingerprint, static_cast<std::uint64_t>(state.mutex_owner.size()));
    for (const auto& [mutex, owner] : state.mutex_owner) {
        append_string(fingerprint, mutex);
        append_u64(fingerprint, owner);
    }

    // As with mutex clocks, rwlock release clocks are HB analysis
    // instrumentation rather than behavioral state. The reader-release
    // accumulator is consumed and reset by WLock, but that bookkeeping still
    // cannot affect control or enabledness. Only occupied rwlocks can, so
    // empty clock-bearing entries canonicalize to absence. Reader holders are
    // already in ascending order.
    std::size_t occupied_rwlocks = 0;
    for (const auto& [_, rwlock] : state.rwlocks) {
        if (rwlock.writer_holder.has_value() || !rwlock.reader_holders.empty()) {
            ++occupied_rwlocks;
        }
    }
    append_u64(fingerprint, static_cast<std::uint64_t>(occupied_rwlocks));
    for (const auto& [name, rwlock] : state.rwlocks) {
        if (!rwlock.writer_holder.has_value() && rwlock.reader_holders.empty()) {
            continue;
        }
        append_string(fingerprint, name);
        append_u64(fingerprint, rwlock.writer_holder.has_value() ? 1U : 0U);
        if (rwlock.writer_holder.has_value()) {
            append_u64(fingerprint, *rwlock.writer_holder);
        }
        append_u64(fingerprint,
                   static_cast<std::uint64_t>(rwlock.reader_holders.size()));
        for (const ThreadId reader : rwlock.reader_holders) {
            append_u64(fingerprint, reader);
        }
    }

    // Permit counts affect SemWait enabledness and therefore belong to exact
    // behavioral state. Zero-count entries canonicalize to absence. The
    // accumulated post-release clock is HB instrumentation and is excluded,
    // just like mutex/rwlock release clocks.
    std::size_t nonzero_semaphores = 0;
    for (const auto& [_, semaphore] : state.semaphores) {
        if (semaphore.permits != 0) {
            ++nonzero_semaphores;
        }
    }
    append_u64(fingerprint, static_cast<std::uint64_t>(nonzero_semaphores));
    for (const auto& [name, semaphore] : state.semaphores) {
        if (semaphore.permits == 0) {
            continue;
        }
        append_string(fingerprint, name);
        append_u64(fingerprint, static_cast<std::uint64_t>(semaphore.permits));
    }

    // A parked arrival changes enabledness while leaving its source PC on the
    // BarrierWait action, so every active generation belongs to exact
    // behavioral state. Empty generations canonicalize to absence. The
    // absolute generation ordinal and accumulated release clock are excluded:
    // they are unobservable occurrence/HB instrumentation and including either
    // would prevent a genuine cyclic barrier loop from closing a lasso.
    std::size_t active_barriers = 0;
    for (const auto& [_, barrier] : state.barriers) {
        if (!barrier.arrivals.empty()) {
            ++active_barriers;
        }
    }
    append_u64(fingerprint, static_cast<std::uint64_t>(active_barriers));
    for (const auto& [name, barrier] : state.barriers) {
        if (barrier.arrivals.empty()) {
            continue;
        }
        append_string(fingerprint, name);
        append_u64(fingerprint, barrier.parties);
        append_u64(fingerprint,
                   static_cast<std::uint64_t>(barrier.arrivals.size()));
        for (const ThreadId arrival : barrier.arrivals) {
            append_u64(fingerprint, arrival);
        }
    }

    // Empty waiter-map entries are observationally identical to absence:
    // Signal/Broadcast treat both as no waiters. Canonicalize them away while
    // preserving each nonempty wait set's deterministic wake order.
    std::size_t nonempty_wait_sets = 0;
    for (const auto& [_, waiters] : state.condition_waiters) {
        if (!waiters.empty()) {
            ++nonempty_wait_sets;
        }
    }
    append_u64(fingerprint, static_cast<std::uint64_t>(nonempty_wait_sets));
    for (const auto& [condition, waiters] : state.condition_waiters) {
        if (waiters.empty()) {
            continue;
        }
        append_string(fingerprint, condition);
        append_u64(fingerprint, static_cast<std::uint64_t>(waiters.size()));
        for (const ThreadId waiter : waiters) {
            append_u64(fingerprint, waiter);
        }
    }

    append_u64(fingerprint, static_cast<std::uint64_t>(state.registers.size()));
    for (const auto& thread_registers : state.registers) {
        for (const Value value : thread_registers) {
            append_u64(fingerprint, static_cast<std::uint64_t>(value));
        }
    }

    append_u64(fingerprint, static_cast<std::uint64_t>(state.wait_phase.size()));
    for (const WaitPhase phase : state.wait_phase) {
        append_u64(fingerprint, static_cast<std::uint64_t>(phase));
    }

    // Missing cells read as zero everywhere in the interpreter. Encoding only
    // nonzero values gives absent and explicitly materialized zero cells one
    // canonical behavioral representation without losing exact value data.
    std::size_t nonzero_cells = 0;
    for (const auto& [_, value] : state.memory_values) {
        if (value != 0) {
            ++nonzero_cells;
        }
    }
    append_u64(fingerprint, static_cast<std::uint64_t>(nonzero_cells));
    for (const auto& [address, value] : state.memory_values) {
        if (value == 0) {
            continue;
        }
        append_string(fingerprint, address);
        append_u64(fingerprint, static_cast<std::uint64_t>(value));
    }

    append_u64(fingerprint, static_cast<std::uint64_t>(state.store_buffers.size()));
    for (const auto& buffer : state.store_buffers) {
        append_u64(fingerprint, static_cast<std::uint64_t>(buffer.size()));
        for (const StoreBufferEntry& entry : buffer) {
            append_string(fingerprint, entry.address);
            append_u64(fingerprint, static_cast<std::uint64_t>(entry.value));
        }
    }

    if (state.memory_model == MemoryModel::PSO) {
        append_u64(fingerprint, static_cast<std::uint64_t>(state.pso_store_buffers.size()));
        for (const PsoAddressBuffer& thread_buffer : state.pso_store_buffers) {
            // std::map iteration is canonical lexicographic address order.
            // Empty queues are erased after their final flush, so the map has
            // exactly one representation for an empty address buffer.
            append_u64(fingerprint, static_cast<std::uint64_t>(thread_buffer.size()));
            for (const auto& [address, values] : thread_buffer) {
                assert(!values.empty());
                append_string(fingerprint, address);
                append_u64(fingerprint, static_cast<std::uint64_t>(values.size()));
                for (const Value value : values) {
                    append_u64(fingerprint, static_cast<std::uint64_t>(value));
                }
            }
        }
    }

    // Excluded deliberately: mutex/rwlock/semaphore/barrier/thread/atomic vector
    // clocks and AddressState race metadata are analysis instrumentation;
    // thread_steps is the exploration budget; schedule is history. None
    // changes program control, enabledness, or modeled values. A repeated
    // fingerprint therefore proves schedule-existence of non-termination
    // only, not repetition of HB/race instrumentation and not fairness.
#if defined(DPOR_EXPLORATION_METRICS)
    auto& metrics = profile_metrics();
    ++metrics.fingerprint_builds;
    metrics.fingerprint_bytes += fingerprint.size();
#endif
    return fingerprint;
}

StateHistory initial_state_history(const ExecutionState& state) {
    StateHistory history;
    history.emplace(behavioral_state_fingerprint(state), state.schedule.size());
    return history;
}

struct PathStateObservation {
    std::optional<std::size_t> cycle_start;
    std::optional<StateHistory::iterator> inserted;
};

PathStateObservation observe_path_behavioral_state(const ExecutionState& state,
                                                   StateHistory& history) {
    StateFingerprint fingerprint = behavioral_state_fingerprint(state);
    const auto first = history.find(fingerprint);
    if (first == history.end()) {
        const auto [position, inserted] =
            history.emplace(std::move(fingerprint), state.schedule.size());
        assert(inserted);
#if defined(DPOR_EXPLORATION_METRICS)
        ++profile_metrics().history_insertions;
#endif
        return PathStateObservation{std::nullopt, position};
    }

    assert(first->second < state.schedule.size());
    return PathStateObservation{first->second, std::nullopt};
}

class PathHistoryRestore {
public:
    PathHistoryRestore(StateHistory& history,
                       std::optional<StateHistory::iterator> inserted)
        : history_(&history), inserted_(inserted) {}

    PathHistoryRestore(const PathHistoryRestore&) = delete;
    PathHistoryRestore& operator=(const PathHistoryRestore&) = delete;

    ~PathHistoryRestore() { restore(); }

    void restore() {
        if (!inserted_.has_value()) {
            return;
        }
        history_->erase(*inserted_);
        inserted_.reset();
#if defined(DPOR_EXPLORATION_METRICS)
        ++profile_metrics().history_restores;
#endif
    }

private:
    StateHistory* history_;
    std::optional<StateHistory::iterator> inserted_;
};

#if defined(DPOR_RESTORE_ASSERTS)
bool deterministically_sample_restore(const Schedule& schedule) {
    if (schedule.size() == 1) {
        return true;
    }

    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&](std::uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    for (const ScheduleStep& step : schedule) {
        mix(step.thread);
        mix(step.action_index);
        mix(step.flush_address.has_value()
                ? static_cast<std::uint64_t>(*step.flush_address) + 1U
                : 0U);
    }
    return (hash & 0x3ffU) == 0;
}

class RestoreExactnessReference {
public:
    RestoreExactnessReference(const ExecutionState& state,
                              const StateHistory& history,
                              const Schedule& schedule)
        : sampled_(deterministically_sample_restore(schedule)) {
        if (sampled_) {
            state_.emplace(state);
            history_.emplace(history);
        }
    }

    void assert_restored(const ExecutionState& state,
                         const StateHistory& history) const {
        if (!sampled_) {
            return;
        }
        assert(state_.has_value() && history_.has_value());
        assert(state == *state_ &&
               "path-history undo changed the complete parent execution state");
        assert(history == *history_ &&
               "path-history undo did not restore the exact reference history");
    }

private:
    bool sampled_{false};
    std::optional<ExecutionState> state_;
    std::optional<StateHistory> history_;
};
#endif

std::optional<std::size_t> observe_behavioral_state(const ExecutionState& state,
                                                    StateHistory& history) {
    return observe_path_behavioral_state(state, history).cycle_start;
}

struct StepReport {
    std::optional<RaceReport> race;
    std::optional<ModelErrorReport> error;
    std::optional<AssertionFailureReport> assertion;
    std::optional<ThreadId> spawned_thread;
};

struct EnabledTransition {
    ScheduleStep endpoint;
    Action effective_action;
    std::optional<std::uint64_t> barrier_generation;
};

struct DporNode {
    std::vector<ScheduleStep> enabled;
    std::map<ScheduleStep, EnabledTransition> enabled_transitions;
    std::vector<ScheduleStep> backtrack;
    std::vector<ScheduleStep> done;
    std::vector<ScheduleStep> sleep;
    std::vector<std::uint32_t> pc;
    std::vector<bool> started;
    std::map<std::string, ThreadId> mutex_owner;
    std::map<std::string, RwLockState> rwlocks;
    std::vector<WaitPhase> wait_phase;
    std::vector<std::deque<StoreBufferEntry>> store_buffers;
    std::vector<PsoAddressBuffer> pso_store_buffers;
    std::map<std::string, BarrierState> barriers;
};

#if defined(DPOR_EXPLORATION_METRICS)
void record_dpor_node_snapshot(const ExecutionState& state) {
    auto& metrics = profile_metrics();
    ++metrics.dpor_node_snapshots;
    metrics.dpor_node_sequence_elements_copied +=
        state.pc.size() + state.started.size() + state.wait_phase.size() +
        state.store_buffers.size() + state.pso_store_buffers.size();
    metrics.dpor_node_map_nodes_copied +=
        state.mutex_owner.size() + state.rwlocks.size() + state.barriers.size();
    for (const auto& [name, rwlock] : state.rwlocks) {
        metrics.dpor_node_map_nodes_copied += rwlock.reader_holders.size();
        metrics.dpor_node_sequence_elements_copied += rwlock.reader_holders.size();
        metrics.dpor_node_clock_components_copied +=
            diagnostic_clock_components(rwlock.writer_release) +
            diagnostic_clock_components(rwlock.reader_releases);
        (void)name;
    }
    for (const auto& buffer : state.store_buffers) {
        metrics.dpor_node_buffer_entries_copied += buffer.size();
        metrics.dpor_node_sequence_elements_copied += buffer.size();
    }
    for (const auto& [name, barrier] : state.barriers) {
        metrics.dpor_node_map_nodes_copied += barrier.arrivals.size();
        metrics.dpor_node_sequence_elements_copied += barrier.arrivals.size();
        metrics.dpor_node_clock_components_copied +=
            diagnostic_clock_components(barrier.arrival_releases);
        (void)name;
    }
    for (const PsoAddressBuffer& buffers : state.pso_store_buffers) {
        metrics.dpor_node_map_nodes_copied += buffers.size();
        for (const auto& [address, values] : buffers) {
            metrics.dpor_node_buffer_entries_copied += values.size();
            metrics.dpor_node_sequence_elements_copied += values.size();
            (void)address;
        }
    }
}
#endif

struct ExecutedTransition {
    ThreadId thread{0};
    Action effective_action;
    ScheduleStep endpoint;
    VectorClock clock;
    std::optional<ThreadId> spawned_thread;
    std::optional<std::uint64_t> barrier_generation;
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

std::vector<std::string> program_addresses(const Program& program) {
    std::set<std::string> addresses;
    for (const auto& thread : program.threads) {
        for (const Action& action : thread) {
            if (!action.address.empty()) {
                addresses.insert(action.address);
            }
        }
    }
    return {addresses.begin(), addresses.end()};
}

std::map<std::string, BarrierState> initial_barriers(const Program& program) {
    std::map<std::string, BarrierState> barriers;
    for (const auto& thread : program.threads) {
        for (const Action& action : thread) {
            if (action.kind != ActionKind::BarrierWait ||
                barriers.find(action.barrier) != barriers.end()) {
                continue;
            }
            BarrierState barrier;
            // Static thread/action order, never execution order, chooses the
            // canonical program-wide count used by forward model errors.
            barrier.parties = action.parties;
            barriers.emplace(action.barrier, std::move(barrier));
        }
    }
    return barriers;
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

ExecutionState initial_state(const Program& program, MemoryModel memory_model) {
    ExecutionState state{
        memory_model,
        std::vector<std::uint32_t>(program.threads.size(), 0),
        initially_started_threads(program),
        {},
        {},
        {},
        {},
        initial_barriers(program),
        {},
        std::vector<VectorClock>(program.threads.size()),
        initial_registers(program.threads.size()),
        std::vector<std::size_t>(program.threads.size(), 0),
        std::vector<WaitPhase>(program.threads.size(), WaitPhase::None),
        {},
        {},
        {},
        std::vector<std::deque<StoreBufferEntry>>(program.threads.size()),
        std::vector<PsoAddressBuffer>(program.threads.size()),
        program_addresses(program),
        {},
    };
    normalize_all_pcs(program, state);
    return state;
}

bool has_next_action(const Program& program, const ExecutionState& state, ThreadId tid) {
    return state.pc.at(tid) < program.threads.at(tid).size();
}

bool is_finished(const Program& program, const ExecutionState& state, ThreadId tid) {
    return state.started.at(tid) &&
           !has_next_action(program, state, tid) &&
           state.store_buffers.at(tid).empty() &&
           state.pso_store_buffers.at(tid).empty();
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

bool requires_cycle_detection(const Program& program) {
    for (ThreadId tid = 0; tid < program.threads.size(); ++tid) {
        const auto& thread = program.threads.at(tid);
        for (std::uint32_t index = 0; index < thread.size(); ++index) {
            const Action& action = thread.at(index);
            if (action.kind != ActionKind::BranchNonzero) {
                continue;
            }
            const auto label_index = resolve_label(program, tid, action.label);
            if (!label_index.has_value()) {
                continue;
            }
            std::uint32_t target = *label_index;
            while (target < thread.size() &&
                   thread.at(target).kind == ActionKind::Label) {
                ++target;
            }
            if (target <= index) {
                return true;
            }
        }
    }
    return false;
}

void advance_pc(const Program& program, ExecutionState& state, ThreadId tid) {
    ++state.pc.at(tid);
    normalize_pc(program, state, tid);
}

void set_pc(const Program& program, ExecutionState& state, ThreadId tid, std::uint32_t pc) {
    state.pc.at(tid) = pc;
    normalize_pc(program, state, tid);
}

bool is_flush_step(const ScheduleStep& step) {
    return step.action_index == kFlushActionIndex;
}

bool is_buffered_memory_model(MemoryModel memory_model) {
    return memory_model == MemoryModel::TSO || memory_model == MemoryModel::PSO;
}

bool has_pending_store(const ExecutionState& state, ThreadId tid) {
    if (state.memory_model == MemoryModel::TSO) {
        return !state.store_buffers.at(tid).empty();
    }
    if (state.memory_model == MemoryModel::PSO) {
        return !state.pso_store_buffers.at(tid).empty();
    }
    return false;
}

bool has_pending_store_at_node(const DporNode& node, ThreadId tid) {
    return !node.store_buffers.at(tid).empty() || !node.pso_store_buffers.at(tid).empty();
}

bool is_buffered_ordered_point(const Action& action) {
    switch (action.kind) {
    case ActionKind::AtomicLoad:
    case ActionKind::AtomicStore:
    case ActionKind::AtomicRmw:
    case ActionKind::CompareExchange:
    case ActionKind::Fence:
    case ActionKind::Lock:
    case ActionKind::TryLock:
    case ActionKind::Unlock:
    case ActionKind::RLock:
    case ActionKind::RUnlock:
    case ActionKind::WLock:
    case ActionKind::WUnlock:
    case ActionKind::SemPost:
    case ActionKind::SemWait:
    case ActionKind::BarrierWait:
    case ActionKind::Wait:
    case ActionKind::Signal:
    case ActionKind::Broadcast:
    case ActionKind::Join:
    case ActionKind::Spawn:
        return true;
    case ActionKind::Set:
    case ActionKind::Label:
    case ActionKind::BranchNonzero:
    case ActionKind::Assert:
    case ActionKind::Read:
    case ActionKind::Write:
    case ActionKind::Flush:
    case ActionKind::Yield:
        return false;
    }
    return false;
}

Action flush_action_for(const StoreBufferEntry& entry) {
    Action action;
    action.kind = ActionKind::Flush;
    action.address = entry.address;
    return action;
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

const RwLockState* find_rwlock(const ExecutionState& state, const std::string& name) {
    const auto rwlock = state.rwlocks.find(name);
    return rwlock == state.rwlocks.end() ? nullptr : &rwlock->second;
}

bool holds_rwlock_read(const RwLockState& rwlock, ThreadId tid) {
    return rwlock.reader_holders.find(tid) != rwlock.reader_holders.end();
}

bool holds_rwlock_write(const RwLockState& rwlock, ThreadId tid) {
    return rwlock.writer_holder.has_value() && *rwlock.writer_holder == tid;
}

bool join_target_is_invalid(const Program& program, ThreadId tid, const Action& action) {
    return action.target >= program.threads.size() || action.target == tid;
}

bool spawn_target_is_invalid(const Program& program, ThreadId tid, const Action& action) {
    return action.target >= program.threads.size() || action.target == tid;
}

bool is_program_action_enabled(const Program& program, const ExecutionState& state, ThreadId tid) {
    if (!state.started.at(tid)) {
        return false;
    }
    if (!has_next_action(program, state, tid)) {
        return false;
    }

    const Action& action = next_action(program, state, tid);
    if (is_buffered_memory_model(state.memory_model) &&
        is_buffered_ordered_point(action) &&
        has_pending_store(state, tid)) {
        return false;
    }

    switch (action.kind) {
    case ActionKind::Label:
        return false;
    case ActionKind::Fence:
        return true;
    case ActionKind::Flush:
        return false;
    case ActionKind::Lock:
        return state.mutex_owner.find(action.mutex) == state.mutex_owner.end();
    case ActionKind::TryLock:
        // Unlike Lock, TryLock never waits for ownership. Under TSO/PSO the
        // ordered-point guard above may still require this thread's buffers to
        // drain before the source transition becomes enabled.
        return true;
    case ActionKind::RLock: {
        const RwLockState* rwlock = find_rwlock(state, action.rwlock);
        if (rwlock == nullptr || !rwlock->writer_holder.has_value()) {
            // An existing read holder remains enabled so execution can report
            // the specified non-reentrant acquisition as a modeled error.
            return true;
        }
        // A read acquisition by the current writer is likewise an executable
        // reentrancy error; another thread's writer blocks it.
        return *rwlock->writer_holder == tid;
    }
    case ActionKind::WLock: {
        const RwLockState* rwlock = find_rwlock(state, action.rwlock);
        if (rwlock == nullptr) {
            return true;
        }
        if (rwlock->writer_holder.has_value()) {
            // Recursive writer acquisition executes to a modeled error.
            return *rwlock->writer_holder == tid;
        }
        // A reader, including this same thread during an attempted upgrade,
        // keeps WLock disabled until every read holder releases.
        return rwlock->reader_holders.empty();
    }
    case ActionKind::SemWait: {
        const auto semaphore = state.semaphores.find(action.semaphore);
        return semaphore != state.semaphores.end() && semaphore->second.permits > 0;
    }
    case ActionKind::BarrierWait: {
        const auto barrier = state.barriers.find(action.barrier);
        assert(barrier != state.barriers.end());
        // Invalid counts remain executable so the forward interpreter reaches
        // a deterministic modeled-error endpoint. A valid participant parks
        // at this source PC after its one arrival transition and cannot arrive
        // again until another thread completes the generation.
        if (action.parties == 0 || action.parties != barrier->second.parties) {
            return true;
        }
        return barrier->second.arrivals.find(tid) ==
               barrier->second.arrivals.end();
    }
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
    case ActionKind::RUnlock:
    case ActionKind::WUnlock:
    case ActionKind::SemPost:
    case ActionKind::Signal:
    case ActionKind::Broadcast:
    case ActionKind::Yield:
        return true;
    }

    return false;
}

bool any_enabled(const Program& program, const ExecutionState& state) {
    for (ThreadId tid = 0; tid < program.threads.size(); ++tid) {
        if (has_pending_store(state, tid) || is_program_action_enabled(program, state, tid)) {
            return true;
        }
    }
    return false;
}

std::vector<ScheduleStep> enabled_steps_for_thread(const Program& program,
                                                   const ExecutionState& state,
                                                   ThreadId tid) {
    std::vector<ScheduleStep> enabled;
    if (!state.started.at(tid)) {
        return enabled;
    }
    if (is_program_action_enabled(program, state, tid)) {
        enabled.push_back(ScheduleStep{tid, state.pc.at(tid), std::nullopt});
    }
    if (has_pending_store(state, tid)) {
        // A pending store buffer entry always has an enabled flush transition.
        // Therefore a thread with pc done but a nonempty buffer is unfinished
        // and cannot create a deadlock solely by buffering.
        if (state.memory_model == MemoryModel::TSO) {
            enabled.push_back(ScheduleStep{tid, kFlushActionIndex, std::nullopt});
        } else {
            assert(state.memory_model == MemoryModel::PSO);
            for (const auto& [address, values] : state.pso_store_buffers.at(tid)) {
                assert(!values.empty());
                const auto found = std::lower_bound(
                    state.flush_addresses.begin(), state.flush_addresses.end(), address);
                assert(found != state.flush_addresses.end() && *found == address);
                const auto address_id = static_cast<std::uint32_t>(
                    std::distance(state.flush_addresses.begin(), found));
                enabled.push_back(ScheduleStep{tid, kFlushActionIndex, address_id});
            }
        }
    }
    return enabled;
}

std::vector<ScheduleStep> enabled_steps(const Program& program, const ExecutionState& state) {
#if defined(DPOR_EXPLORATION_METRICS)
    auto& metrics = profile_metrics();
    ++metrics.enabled_collections;
    metrics.enabled_thread_probes += program.threads.size();
#endif
    std::vector<ScheduleStep> enabled;
    for (ThreadId tid = 0; tid < program.threads.size(); ++tid) {
        // INVARIANTS.md Replay/HB: Wait's two phases are represented in
        // per-thread state, not by rewriting the program. A sleeping waiter is
        // absent, and a woken waiter appears only when the mutex reacquire edge
        // can run under the original (thread, action_index) schedule step.
        // Join appears only when its target is finished, except invalid joins
        // stay enabled so replay reaches the modeled error deterministically.
        const auto thread_steps = enabled_steps_for_thread(program, state, tid);
        enabled.insert(enabled.end(), thread_steps.begin(), thread_steps.end());
    }
    std::sort(enabled.begin(), enabled.end());
#if defined(DPOR_EXPLORATION_METRICS)
    metrics.enabled_steps_emitted += enabled.size();
#endif
    return enabled;
}

Action effective_action_for_step(const Program& program,
                                 const ExecutionState& state,
                                 const ScheduleStep& step) {
    if (is_flush_step(step)) {
        assert(has_pending_store(state, step.thread));
        if (state.memory_model == MemoryModel::TSO) {
            assert(!step.flush_address.has_value());
            return flush_action_for(state.store_buffers.at(step.thread).front());
        }
        assert(state.memory_model == MemoryModel::PSO);
        assert(step.flush_address.has_value());
        assert(*step.flush_address < state.flush_addresses.size());
        Action action;
        action.kind = ActionKind::Flush;
        action.address = state.flush_addresses.at(*step.flush_address);
        const auto pending = state.pso_store_buffers.at(step.thread).find(action.address);
        assert(pending != state.pso_store_buffers.at(step.thread).end());
        assert(!pending->second.empty());
        (void)pending;
        return action;
    }
    return effective_next_action(program, state, step.thread);
}

std::optional<std::uint64_t> barrier_generation_for(
    const ExecutionState& state,
    const Action& action) {
    if (action.kind != ActionKind::BarrierWait) {
        return std::nullopt;
    }
    const auto barrier = state.barriers.find(action.barrier);
    assert(barrier != state.barriers.end());
    return barrier->second.generation;
}

std::map<ScheduleStep, EnabledTransition> enabled_transitions(const Program& program,
                                                              const ExecutionState& state,
                                                              const std::vector<ScheduleStep>& enabled) {
#if defined(DPOR_EXPLORATION_METRICS)
    auto& metrics = profile_metrics();
    ++metrics.dpor_enabled_transition_maps;
#endif
    std::map<ScheduleStep, EnabledTransition> transitions;
    for (const ScheduleStep& step : enabled) {
#if defined(DPOR_EXPLORATION_METRICS)
        Action action = effective_action_for_step(program, state, step);
        const auto barrier_generation = barrier_generation_for(state, action);
        ++metrics.dpor_enabled_transition_entries;
        ++metrics.effective_actions_materialized;
        metrics.action_string_bytes_materialized += diagnostic_action_string_bytes(action);
        transitions.emplace(
            step,
            EnabledTransition{
                step,
                std::move(action),
                barrier_generation,
            });
#else
        Action action = effective_action_for_step(program, state, step);
        transitions.emplace(
            step,
            EnabledTransition{
                step,
                action,
                barrier_generation_for(state, action),
            });
#endif
    }
    return transitions;
}

std::map<ScheduleStep, EnabledTransition> enabled_transitions(const Program& program,
                                                              const ExecutionState& state) {
    const std::vector<ScheduleStep> enabled = enabled_steps(program, state);
    return enabled_transitions(program, state, enabled);
}

bool contains_step(const std::vector<ScheduleStep>& steps, const ScheduleStep& step) {
    return std::binary_search(steps.begin(), steps.end(), step);
}

void insert_step(std::vector<ScheduleStep>& steps, const ScheduleStep& step) {
    const auto position = std::lower_bound(steps.begin(), steps.end(), step);
    if (position == steps.end() || !(*position == step)) {
        steps.insert(position, step);
    }
}

void remove_step(std::vector<ScheduleStep>& steps, const ScheduleStep& step) {
    const auto position = std::lower_bound(steps.begin(), steps.end(), step);
    if (position != steps.end() && *position == step) {
        steps.erase(position);
    }
}

bool transition_enabled_at_node(const DporNode& node, const ExecutedTransition& transition) {
    const auto enabled = node.enabled_transitions.find(transition.endpoint);
    return enabled != node.enabled_transitions.end() &&
           enabled->second.endpoint == transition.endpoint &&
           enabled->second.effective_action == transition.effective_action &&
           enabled->second.barrier_generation == transition.barrier_generation;
}

bool has_next_action_at_node(const Program& program, const DporNode& node, ThreadId tid) {
    return node.pc.at(tid) < program.threads.at(tid).size();
}

bool is_finished_at_node(const Program& program, const DporNode& node, ThreadId tid) {
    return node.started.at(tid) &&
           !has_next_action_at_node(program, node, tid) &&
           !has_pending_store_at_node(node, tid);
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

std::vector<ScheduleStep> enabled_steps_for_thread_at_node(const DporNode& node, ThreadId tid) {
    std::vector<ScheduleStep> steps;
    for (const ScheduleStep& step : node.enabled) {
        if (step.thread == tid) {
            steps.push_back(step);
        }
    }
    return steps;
}

bool enabled_at_node(const DporNode& node, ThreadId tid) {
    const auto steps = enabled_steps_for_thread_at_node(node, tid);
    return !steps.empty();
}

void append_steps(std::vector<ScheduleStep>& destination, const std::vector<ScheduleStep>& source) {
    for (const ScheduleStep& step : source) {
        insert_step(destination, step);
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

std::optional<std::vector<ScheduleStep>> enabler_heads_for_thread(const Program& program,
                                                                  const DporNode& node,
                                                                  ThreadId tid,
                                                                  std::vector<bool>& visiting);

std::optional<std::vector<ScheduleStep>> enabler_heads_for_spawn_target(const Program& program,
                                                                        const DporNode& node,
                                                                        ThreadId target,
                                                                        std::vector<bool>& visiting) {
    std::vector<ScheduleStep> heads;
    for (ThreadId source = 0; source < program.threads.size(); ++source) {
        if (!has_remaining_spawn_to(program, node, source, target)) {
            continue;
        }

        const auto source_heads = enabler_heads_for_thread(program, node, source, visiting);
        if (!source_heads.has_value()) {
            return std::nullopt;
        }
        append_steps(heads, *source_heads);
    }

    if (heads.empty()) {
        return std::nullopt;
    }
    return heads;
}

std::optional<std::vector<ScheduleStep>> enabler_heads_for_waiter(const Program& program,
                                                                  const DporNode& node,
                                                                  const Action& wait_action,
                                                                  std::vector<bool>& visiting) {
    std::vector<ScheduleStep> heads;
    for (ThreadId source = 0; source < program.threads.size(); ++source) {
        if (!has_remaining_wake_on(program, node, source, wait_action.condition)) {
            continue;
        }

        const auto source_heads = enabler_heads_for_thread(program, node, source, visiting);
        if (!source_heads.has_value()) {
            return std::nullopt;
        }
        append_steps(heads, *source_heads);
    }

    if (heads.empty()) {
        return std::nullopt;
    }
    return heads;
}

std::optional<std::vector<ScheduleStep>> enabler_heads_for_mutex_owner(const Program& program,
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

std::optional<std::vector<ScheduleStep>> enabler_heads_for_rwlock_writer(
    const Program& program,
    const DporNode& node,
    const std::string& name,
    ThreadId blocked_thread,
    std::vector<bool>& visiting) {
    const auto found = node.rwlocks.find(name);
    if (found == node.rwlocks.end() || !found->second.writer_holder.has_value() ||
        *found->second.writer_holder == blocked_thread) {
        return std::nullopt;
    }
    return enabler_heads_for_thread(
        program, node, *found->second.writer_holder, visiting);
}

std::optional<std::vector<ScheduleStep>> enabler_heads_for_rwlock_readers(
    const Program& program,
    const DporNode& node,
    const std::string& name,
    ThreadId blocked_thread,
    std::vector<bool>& visiting) {
    const auto found = node.rwlocks.find(name);
    if (found == node.rwlocks.end() || found->second.reader_holders.empty()) {
        return std::nullopt;
    }

    std::vector<ScheduleStep> heads;
    for (const ThreadId reader : found->second.reader_holders) {
        // A read-to-write upgrade is a deliberate self wait. A recursive
        // enabler chain cannot discharge it, so use the caller's conservative
        // all-enabled fallback instead.
        if (reader == blocked_thread) {
            return std::nullopt;
        }
        const auto reader_heads =
            enabler_heads_for_thread(program, node, reader, visiting);
        if (!reader_heads.has_value()) {
            return std::nullopt;
        }
        append_steps(heads, *reader_heads);
    }
    return heads.empty()
               ? std::nullopt
               : std::optional<std::vector<ScheduleStep>>{std::move(heads)};
}

std::optional<std::vector<ScheduleStep>> enabler_heads_for_thread(const Program& program,
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
        const auto heads = enabled_steps_for_thread_at_node(node, tid);
        clear_visiting();
        return heads;
    }

    const Action static_action = next_action_at_node(program, node, tid);
    const Action effective_action = effective_next_action_at_node(program, node, tid);
    std::optional<std::vector<ScheduleStep>> heads;
    switch (effective_action.kind) {
    case ActionKind::Lock:
        heads = enabler_heads_for_mutex_owner(program, node, effective_action.mutex, tid, visiting);
        break;
    case ActionKind::RLock:
        heads = enabler_heads_for_rwlock_writer(
            program, node, effective_action.rwlock, tid, visiting);
        break;
    case ActionKind::WLock: {
        const auto rwlock = node.rwlocks.find(effective_action.rwlock);
        if (rwlock != node.rwlocks.end() && rwlock->second.writer_holder.has_value()) {
            heads = enabler_heads_for_rwlock_writer(
                program, node, effective_action.rwlock, tid, visiting);
        } else {
            heads = enabler_heads_for_rwlock_readers(
                program, node, effective_action.rwlock, tid, visiting);
        }
        break;
    }
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
    case ActionKind::SemWait:
        // Any remaining post on this name could enable the wait. There is no
        // unique ownership chain to follow, so preserve the caller's
        // conservative all-enabled fallback.
        break;
    case ActionKind::BarrierWait:
        // Any not-yet-arrived participant on this name may complete the
        // generation. There is no unique enabler, so keep the conservative
        // all-enabled fallback.
        break;
    default:
        break;
    }

    clear_visiting();
    return heads;
}

std::optional<std::vector<ScheduleStep>> disabled_repair_steps(const Program& program,
                                                               const DporNode& node,
                                                               const ExecutedTransition& current) {
    if (current.thread >= program.threads.size()) {
        return std::nullopt;
    }

    std::vector<bool> visiting(program.threads.size(), false);
    if (!node.started.at(current.thread)) {
        return enabler_heads_for_spawn_target(program, node, current.thread, visiting);
    }

    if (node.pc.at(current.thread) < current.endpoint.action_index) {
        return enabler_heads_for_thread(program, node, current.thread, visiting);
    }

    if (!has_next_action_at_node(program, node, current.thread)) {
        return std::nullopt;
    }

    if (node.pc.at(current.thread) != current.endpoint.action_index) {
        return std::nullopt;
    }

    const Action static_action = next_action_at_node(program, node, current.thread);
    const Action effective_action = effective_next_action_at_node(program, node, current.thread);
    switch (effective_action.kind) {
    case ActionKind::RLock:
        if (static_action == current.effective_action) {
            return enabler_heads_for_rwlock_writer(
                program, node, static_action.rwlock, current.thread, visiting);
        }
        break;
    case ActionKind::WLock:
        if (static_action == current.effective_action) {
            const auto rwlock = node.rwlocks.find(static_action.rwlock);
            if (rwlock != node.rwlocks.end() &&
                rwlock->second.writer_holder.has_value()) {
                return enabler_heads_for_rwlock_writer(
                    program, node, static_action.rwlock, current.thread, visiting);
            }
            return enabler_heads_for_rwlock_readers(
                program, node, static_action.rwlock, current.thread, visiting);
        }
        break;
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
    case ActionKind::SemWait:
        // Anonymous permits do not identify one concrete posting enabler.
        // Returning no narrowed repair keeps every enabled candidate poster.
        break;
    case ActionKind::BarrierWait:
        // An incomplete generation likewise has no unique last-arrival
        // enabler. Returning no narrowed repair keeps every enabled candidate.
        break;
    default:
        break;
    }

    return std::nullopt;
}

void add_repair_steps(DporNode& node, const std::vector<ScheduleStep>& steps) {
    for (const ScheduleStep& step : steps) {
        if (!contains_step(node.enabled, step)) {
            continue;
        }
        insert_step(node.backtrack, step);
        remove_step(node.sleep, step);
    }
}

void add_all_enabled_repair_steps(DporNode& node) {
    add_repair_steps(node, node.enabled);
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

bool is_reader_rwlock_action(const Action& action) {
    return action.kind == ActionKind::RLock || action.kind == ActionKind::RUnlock;
}

bool program_has_writer_action_for_rwlock(const Program& program,
                                          const std::string& name) {
    for (const auto& thread : program.threads) {
        for (const Action& action : thread) {
            if ((action.kind == ActionKind::WLock ||
                 action.kind == ActionKind::WUnlock) &&
                action.rwlock == name) {
                return true;
            }
        }
    }
    return false;
}

bool transitions_independent(const Program& program,
                             MemoryModel memory_model,
                             ThreadId lhs_thread,
                             const Action& lhs,
                             ThreadId rhs_thread,
                             const Action& rhs) {
#if defined(DPOR_EXPLORATION_METRICS)
    ++profile_metrics().dpor_independence_checks;
#endif
    if (lhs_thread == rhs_thread &&
        lhs.kind == ActionKind::Flush &&
        rhs.kind == ActionKind::Flush &&
        !lhs.address.empty() &&
        !rhs.address.empty() &&
        lhs.address != rhs.address) {
        // PSO flushes are not source-program actions. Different-address
        // flushes of one owner commute in modeled state and are independently
        // schedulable; initialize_dpor_backtrack nevertheless forces every
        // co-enabled sibling address into the persistent choice set so trace
        // DPOR cannot mistake them for same-thread program order.
        return true;
    }
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

    if (is_reader_rwlock_action(lhs) && is_reader_rwlock_action(rhs) &&
        lhs.rwlock == rhs.rwlock &&
        !program_has_writer_action_for_rwlock(program, lhs.rwlock)) {
        // In a statically writer-free rwlock, cross-thread reader acquisitions
        // and releases form a commuting diamond: the final sorted holder set
        // is identical; accumulated release clocks join commutatively; RLock
        // never consumes that accumulator; and no shared value or race record
        // changes. Crucially, the whole-program exclusion of both WLock and
        // WUnlock removes the middle-writer witness that could otherwise be
        // enabled after one release order but not the other. This refinement
        // is checker-local because the public action predicate lacks program
        // context. RLock/RLock remains independently justified even when a
        // writer exists and falls through to the public predicate below.
        return true;
    }

    if (memory_model != MemoryModel::SC &&
        (lhs.kind == ActionKind::Write || rhs.kind == ActionKind::Write) &&
        lhs.kind != ActionKind::Spawn &&
        rhs.kind != ActionKind::Spawn) {
        // Under TSO/PSO a source Write is only a private enqueue: it advances
        // its owner's pc/clock and appends to that owner's buffer, which the
        // other thread cannot touch. Executing the two enabled transitions in
        // either order therefore leaves identical buffers, shared state, race
        // metadata, and enabledness. The later Flush remains the globally
        // visible write and retains same-address dependencies with reads,
        // atomics, and visible flushes. SC and same-thread pairs cannot reach
        // this clause, while Spawn retains its conservative enabledness edge.
        return true;
    }

    return independent(lhs, rhs);
}

bool node_enabled_transition_matches(
    const DporNode& node,
    ThreadId thread,
    const Action& action,
    const ScheduleStep& endpoint,
    std::optional<std::uint64_t> barrier_generation) {
    if (endpoint.thread != thread) {
        return false;
    }
    const auto enabled = node.enabled_transitions.find(endpoint);
    return enabled != node.enabled_transitions.end() &&
           enabled->second.endpoint == endpoint &&
           enabled->second.effective_action == action &&
           enabled->second.barrier_generation == barrier_generation;
}

bool same_mutex_try_locks_independent_at_node(
    const DporNode& node,
    ThreadId lhs_thread,
    const Action& lhs,
    const ScheduleStep& lhs_endpoint,
    std::optional<std::uint64_t> lhs_generation,
    ThreadId rhs_thread,
    const Action& rhs,
    const ScheduleStep& rhs_endpoint,
    std::optional<std::uint64_t> rhs_generation) {
    if (lhs_thread == rhs_thread || lhs.kind != ActionKind::TryLock ||
        rhs.kind != ActionKind::TryLock || lhs.mutex.empty() ||
        lhs.mutex != rhs.mutex || lhs_generation.has_value() ||
        rhs_generation.has_value() ||
        !node_enabled_transition_matches(
            node, lhs_thread, lhs, lhs_endpoint, lhs_generation) ||
        !node_enabled_transition_matches(
            node, rhs_thread, rhs, rhs_endpoint, rhs_generation)) {
        return false;
    }

    const auto owner = node.mutex_owner.find(lhs.mutex);
    if (owner == node.mutex_owner.end() || owner->second == lhs_thread ||
        owner->second == rhs_thread) {
        return false;
    }

    // Both TryLocks fail. Each changes only its own thread's tick, pc, and
    // destination register (r0 by default); those effects are disjoint.
    // Neither changes the owner/release state, shared values, or race state,
    // and the exact co-enabled other TryLock stays enabled after either order.
    return true;
}

bool same_barrier_arrivals_independent_at_node(
    const DporNode& node,
    ThreadId lhs_thread,
    const Action& lhs,
    const ScheduleStep& lhs_endpoint,
    std::optional<std::uint64_t> lhs_generation,
    ThreadId rhs_thread,
    const Action& rhs,
    const ScheduleStep& rhs_endpoint,
    std::optional<std::uint64_t> rhs_generation) {
    assert(lhs.kind == ActionKind::BarrierWait);
    assert(rhs.kind == ActionKind::BarrierWait);
    assert(lhs.barrier == rhs.barrier);

    const auto barrier = node.barriers.find(lhs.barrier);
    if (barrier == node.barriers.end() || lhs.parties == 0 ||
        lhs.parties != barrier->second.parties ||
        rhs.parties != barrier->second.parties || !lhs_generation.has_value() ||
        lhs_generation != rhs_generation ||
        *lhs_generation != barrier->second.generation ||
        !node_enabled_transition_matches(
            node, lhs_thread, lhs, lhs_endpoint, lhs_generation) ||
        !node_enabled_transition_matches(
            node, rhs_thread, rhs, rhs_endpoint, rhs_generation) ||
        barrier->second.arrivals.find(lhs_thread) !=
            barrier->second.arrivals.end() ||
        barrier->second.arrivals.find(rhs_thread) !=
            barrier->second.arrivals.end()) {
        return false;
    }

    // Both orders insert exactly the same two thread IDs and join the same two
    // clocks. They commute only while neither second arrival can complete the
    // generation. Equality is deliberately dependent: whichever transition
    // is second performs the all-participant release and changes enabledness.
    return barrier->second.arrivals.size() + 2 < barrier->second.parties;
}

bool barrier_arrival_releases_parked_thread(
    const DporNode& node,
    ThreadId barrier_thread,
    const Action& barrier_action,
    const ScheduleStep& barrier_endpoint,
    std::optional<std::uint64_t> barrier_generation,
    ThreadId other_thread) {
    if (barrier_action.kind != ActionKind::BarrierWait ||
        !node_enabled_transition_matches(node,
                                         barrier_thread,
                                         barrier_action,
                                         barrier_endpoint,
                                         barrier_generation)) {
        return false;
    }
    const auto barrier = node.barriers.find(barrier_action.barrier);
    if (barrier == node.barriers.end() || barrier_action.parties == 0 ||
        barrier_action.parties != barrier->second.parties ||
        barrier_generation !=
            std::optional<std::uint64_t>{barrier->second.generation}) {
        return false;
    }
    return barrier->second.arrivals.size() + 1 == barrier->second.parties &&
           barrier->second.arrivals.find(other_thread) !=
               barrier->second.arrivals.end();
}

bool transitions_independent_at_node(
    const Program& program,
    MemoryModel memory_model,
    const DporNode& node,
    ThreadId lhs_thread,
    const Action& lhs,
    const ScheduleStep& lhs_endpoint,
    std::optional<std::uint64_t> lhs_generation,
    ThreadId rhs_thread,
    const Action& rhs,
    const ScheduleStep& rhs_endpoint,
    std::optional<std::uint64_t> rhs_generation) {
    if (lhs.kind == ActionKind::TryLock &&
        rhs.kind == ActionKind::TryLock && lhs.mutex == rhs.mutex) {
#if defined(DPOR_EXPLORATION_METRICS)
        ++profile_metrics().dpor_independence_checks;
#endif
        return same_mutex_try_locks_independent_at_node(
            node,
            lhs_thread,
            lhs,
            lhs_endpoint,
            lhs_generation,
            rhs_thread,
            rhs,
            rhs_endpoint,
            rhs_generation);
    }

    if (lhs.kind == ActionKind::BarrierWait &&
        rhs.kind == ActionKind::BarrierWait && lhs.barrier == rhs.barrier) {
#if defined(DPOR_EXPLORATION_METRICS)
        ++profile_metrics().dpor_independence_checks;
#endif
        return same_barrier_arrivals_independent_at_node(
            node,
            lhs_thread,
            lhs,
            lhs_endpoint,
            lhs_generation,
            rhs_thread,
            rhs,
            rhs_endpoint,
            rhs_generation);
    }

    // A last arrival advances every already-parked participant to its next
    // source action and joins its clock. That enabledness/HB edge makes the
    // release transition dependent with such a participant's later action,
    // even though the public action-only predicate cannot see the parked set.
    const bool release_changes_enabledness =
        barrier_arrival_releases_parked_thread(node,
                                               lhs_thread,
                                               lhs,
                                               lhs_endpoint,
                                               lhs_generation,
                                               rhs_thread) ||
        barrier_arrival_releases_parked_thread(node,
                                               rhs_thread,
                                               rhs,
                                               rhs_endpoint,
                                               rhs_generation,
                                               lhs_thread);
    if (release_changes_enabledness) {
#if defined(DPOR_EXPLORATION_METRICS)
        ++profile_metrics().dpor_independence_checks;
#endif
        return false;
    }

    return transitions_independent(
        program, memory_model, lhs_thread, lhs, rhs_thread, rhs);
}

std::optional<ScheduleStep> next_unexplored_backtrack(const DporNode& node) {
    for (const ScheduleStep& step : node.backtrack) {
        if (!contains_step(node.done, step)) {
            return step;
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
#if defined(DPOR_EXPLORATION_METRICS)
    record_report_schedule_copy(schedule);
#endif
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
#if defined(DPOR_EXPLORATION_METRICS)
    record_report_schedule_copy(schedule);
#endif
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

ModelErrorReport make_rwlock_unlock_error(const Action& action,
                                          ScheduleStep endpoint,
                                          const Schedule& schedule) {
#if defined(DPOR_EXPLORATION_METRICS)
    record_report_schedule_copy(schedule);
#endif
    std::ostringstream message;
    message << "thread " << endpoint.thread << " attempted to "
            << (action.kind == ActionKind::RUnlock ? "read-unlock" : "write-unlock")
            << " rwlock '" << action.rwlock
            << "' but it does not hold that mode";
    return ModelErrorReport{endpoint, message.str(), schedule};
}

ModelErrorReport make_rwlock_reentrancy_error(const Action& action,
                                              ScheduleStep endpoint,
                                              const Schedule& schedule) {
#if defined(DPOR_EXPLORATION_METRICS)
    record_report_schedule_copy(schedule);
#endif
    std::ostringstream message;
    message << "thread " << endpoint.thread << " attempted a reentrant "
            << (action.kind == ActionKind::RLock ? "read" : "write")
            << " acquisition of rwlock '" << action.rwlock << "'";
    return ModelErrorReport{endpoint, message.str(), schedule};
}

ModelErrorReport make_join_error(const Action& action, ScheduleStep endpoint, const Schedule& schedule) {
#if defined(DPOR_EXPLORATION_METRICS)
    record_report_schedule_copy(schedule);
#endif
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
#if defined(DPOR_EXPLORATION_METRICS)
    record_report_schedule_copy(schedule);
#endif
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
#if defined(DPOR_EXPLORATION_METRICS)
    record_report_schedule_copy(schedule);
#endif
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
#if defined(DPOR_EXPLORATION_METRICS)
    record_report_schedule_copy(schedule);
#endif
    std::ostringstream message;
    message << "thread " << endpoint.thread << " branched to unknown label '" << action.label << "'";
    return ModelErrorReport{endpoint, message.str(), schedule};
}

ModelErrorReport make_barrier_error(const Action& action,
                                    std::uint32_t required_parties,
                                    ScheduleStep endpoint,
                                    const Schedule& schedule) {
#if defined(DPOR_EXPLORATION_METRICS)
    record_report_schedule_copy(schedule);
#endif
    std::ostringstream message;
    message << "thread " << endpoint.thread << " attempted to wait on barrier '"
            << action.barrier << "' with " << action.parties << " parties";
    if (action.parties == 0) {
        message << ", but parties must be greater than zero";
    } else {
        message << ", but the program-wide barrier requires "
                << required_parties;
    }
    return ModelErrorReport{endpoint, message.str(), schedule};
}

AssertionFailureReport make_assertion_failure(const Action& action,
                                              ScheduleStep endpoint,
                                              const Schedule& schedule,
                                              const ExecutionState& state) {
#if defined(DPOR_EXPLORATION_METRICS)
    record_report_schedule_copy(schedule);
#endif
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

Value read_plain_value(const ExecutionState& state, ThreadId tid, const std::string& address) {
    if (state.memory_model == MemoryModel::TSO) {
        const auto& buffer = state.store_buffers.at(tid);
        for (auto iter = buffer.rbegin(); iter != buffer.rend(); ++iter) {
            if (iter->address == address) {
                return iter->value;
            }
        }
    } else if (state.memory_model == MemoryModel::PSO) {
        const auto pending = state.pso_store_buffers.at(tid).find(address);
        if (pending != state.pso_store_buffers.at(tid).end()) {
            assert(!pending->second.empty());
            return pending->second.back();
        }
    }
    const auto value = state.memory_values.find(address);
    if (value == state.memory_values.end()) {
        return 0;
    }
    return value->second;
}

StepReport execute_enabled_step(const Program& program, ExecutionState& state, const ScheduleStep& step) {
#if defined(DPOR_EXPLORATION_METRICS)
    ++profile_metrics().executed_steps;
#endif
    const ThreadId tid = step.thread;
    const ScheduleStep endpoint = step;
    Action flush_action;
    const Action* action_ptr = nullptr;
    if (is_flush_step(step)) {
        assert(is_buffered_memory_model(state.memory_model));
        assert(has_pending_store(state, tid));
        flush_action = effective_action_for_step(program, state, step);
        action_ptr = &flush_action;
    } else {
        assert(step.action_index == state.pc.at(tid));
        action_ptr = &program.threads.at(tid).at(step.action_index);
    }
    const Action& action = *action_ptr;

#if defined(DPOR_EXPLORATION_METRICS)
    ++profile_metrics().schedule_pushes;
#endif
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
    case ActionKind::TryLock: {
        advance_pc(program, state, tid);
        const bool success =
            state.mutex_owner.find(action.mutex) == state.mutex_owner.end();
        write_register(state, tid, action.destination.value_or(0), success ? 1 : 0);
        if (success) {
            // The success path is exactly Lock's acquire edge. Failure changes
            // only this thread's destination register and must not join either
            // the stored release frontier or the live owner's clock.
            state.mutex_owner[action.mutex] = tid;
            state.thread_clock.at(tid).join(state.mutex_clock[action.mutex]);
        }
        break;
    }
    case ActionKind::RLock: {
        advance_pc(program, state, tid);
        const auto found = state.rwlocks.find(action.rwlock);
        if (found != state.rwlocks.end() &&
            (holds_rwlock_read(found->second, tid) ||
             holds_rwlock_write(found->second, tid))) {
            report.error =
                make_rwlock_reentrancy_error(action, endpoint, state.schedule);
            break;
        }

        RwLockState& rwlock = state.rwlocks[action.rwlock];
        assert(!rwlock.writer_holder.has_value());
        // A reader acquires exactly the last writer release. It deliberately
        // does not join accumulated reader releases, which would fabricate
        // reader-to-reader HB and could hide races between readers.
        state.thread_clock.at(tid).join(rwlock.writer_release);
        const bool inserted = rwlock.reader_holders.insert(tid).second;
        assert(inserted);
        (void)inserted;
        break;
    }
    case ActionKind::WLock: {
        advance_pc(program, state, tid);
        const auto found = state.rwlocks.find(action.rwlock);
        if (found != state.rwlocks.end() &&
            holds_rwlock_write(found->second, tid)) {
            report.error =
                make_rwlock_reentrancy_error(action, endpoint, state.schedule);
            break;
        }

        RwLockState& rwlock = state.rwlocks[action.rwlock];
        // A read-to-write upgrade is never executable: enabledness keeps it
        // blocked on its own read hold so deadlock reporting can tag self_wait.
        assert(!holds_rwlock_read(rwlock, tid));
        assert(!rwlock.writer_holder.has_value());
        assert(rwlock.reader_holders.empty());
        // The writer acquires both publication frontiers. Reader releases are
        // accumulated because no single release necessarily represents all
        // readers that drained; consuming and resetting the accumulator here
        // starts the next reader epoch without adding reader-reader edges.
        state.thread_clock.at(tid).join(rwlock.writer_release);
        state.thread_clock.at(tid).join(rwlock.reader_releases);
        rwlock.reader_releases = VectorClock{};
        rwlock.writer_holder = tid;
        break;
    }
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
    case ActionKind::RUnlock: {
        advance_pc(program, state, tid);
        const auto found = state.rwlocks.find(action.rwlock);
        if (found == state.rwlocks.end() ||
            !holds_rwlock_read(found->second, tid)) {
            report.error =
                make_rwlock_unlock_error(action, endpoint, state.schedule);
            break;
        }

        // Each read release contributes its post-tick clock to the current
        // reader epoch. This does not affect a later RLock; only WLock consumes
        // the accumulated join after all readers have drained.
        found->second.reader_releases.join(state.thread_clock.at(tid));
        found->second.reader_holders.erase(tid);
        break;
    }
    case ActionKind::WUnlock: {
        advance_pc(program, state, tid);
        const auto found = state.rwlocks.find(action.rwlock);
        if (found == state.rwlocks.end() ||
            !holds_rwlock_write(found->second, tid)) {
            report.error =
                make_rwlock_unlock_error(action, endpoint, state.schedule);
            break;
        }

        // Writer release replaces, rather than joins, the previous writer
        // frontier. The writer's acquired clock already contains everything
        // from the preceding writer and reader epoch.
        found->second.writer_release = state.thread_clock.at(tid);
        found->second.writer_holder.reset();
        break;
    }
    case ActionKind::SemPost: {
        advance_pc(program, state, tid);
        SemaphoreState& semaphore = state.semaphores[action.semaphore];
        // The modeled semaphore has no ceiling. The finite interpreter's
        // per-thread step bound makes machine-size overflow unreachable in a
        // realizable exploration; assert that implementation boundary rather
        // than inventing a disabled/error state.
        assert(semaphore.permits < std::numeric_limits<std::size_t>::max());
        ++semaphore.permits;
        semaphore.post_releases.join(state.thread_clock.at(tid));
        break;
    }
    case ActionKind::SemWait: {
        advance_pc(program, state, tid);
        const auto found = state.semaphores.find(action.semaphore);
        assert(found != state.semaphores.end());
        assert(found->second.permits > 0);
        --found->second.permits;
        // Strong-semaphore acquire: permits are anonymous, so a successful
        // wait joins the accumulated release clocks of every prior post. The
        // accumulator is intentionally neither cleared nor replaced.
        state.thread_clock.at(tid).join(found->second.post_releases);
        break;
    }
    case ActionKind::BarrierWait: {
        BarrierState& barrier = state.barriers.at(action.barrier);
        if (action.parties == 0 || action.parties != barrier.parties) {
            advance_pc(program, state, tid);
            report.error = make_barrier_error(
                action, barrier.parties, endpoint, state.schedule);
            break;
        }

        const bool inserted = barrier.arrivals.insert(tid).second;
        assert(inserted && "one thread arrived twice in one barrier generation");
        (void)inserted;
        barrier.arrival_releases.join(state.thread_clock.at(tid));
        if (barrier.arrivals.size() < barrier.parties) {
            // A non-last arrival remains at this source action. Enabledness
            // excludes it until the last participant atomically releases the
            // whole generation and advances every parked PC.
            break;
        }

        assert(barrier.arrivals.size() == barrier.parties);
        const std::vector<ThreadId> participants(
            barrier.arrivals.begin(), barrier.arrivals.end());
        const VectorClock release_clock = barrier.arrival_releases;
        for (const ThreadId participant : participants) {
            assert(state.started.at(participant));
            assert(has_next_action(program, state, participant));
            const Action& participant_action = next_action(program, state, participant);
            assert(participant_action.kind == ActionKind::BarrierWait);
            assert(participant_action.barrier == action.barrier);
            assert(participant_action.parties == barrier.parties);
            (void)participant_action;
            state.thread_clock.at(participant).join(release_clock);
            advance_pc(program, state, participant);
        }

        barrier.arrivals.clear();
        barrier.arrival_releases = VectorClock{};
        assert(barrier.generation < std::numeric_limits<std::uint64_t>::max());
        ++barrier.generation;
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
    case ActionKind::Fence:
        advance_pc(program, state, tid);
        break;
    case ActionKind::Flush: {
        assert(is_buffered_memory_model(state.memory_model));
        StoreBufferEntry entry;
        if (state.memory_model == MemoryModel::TSO) {
            auto& buffer = state.store_buffers.at(tid);
            assert(!buffer.empty());
            entry = buffer.front();
            buffer.pop_front();
        } else {
            assert(step.flush_address.has_value());
            assert(*step.flush_address < state.flush_addresses.size());
            const std::string& address = state.flush_addresses.at(*step.flush_address);
            auto& buffers = state.pso_store_buffers.at(tid);
            const auto pending = buffers.find(address);
            assert(pending != buffers.end());
            assert(!pending->second.empty());
            entry = StoreBufferEntry{address, pending->second.front()};
            pending->second.pop_front();
            if (pending->second.empty()) {
                buffers.erase(pending);
            }
        }
        state.memory_values[entry.address] = entry.value;
        Action committed;
        committed.kind = ActionKind::Flush;
        committed.address = entry.address;
        // Buffered-model visibility point: an enqueued plain write becomes globally
        // visible only at this flush transition, so the race endpoint and
        // vector-clock comparison use the flush step, not the earlier enqueue.
        report.race = record_write(
            state,
            committed,
            MemoryAccess{state.thread_clock.at(tid), endpoint, false, true});
        break;
    }
    case ActionKind::Read:
        advance_pc(program, state, tid);
        if (!action.address.empty()) {
            write_register(state, tid, action.destination, read_plain_value(state, tid, action.address));
            report.race = record_read(
                state,
                action,
                MemoryAccess{state.thread_clock.at(tid), endpoint, false, false});
        }
        break;
    case ActionKind::Write:
        advance_pc(program, state, tid);
        if (!action.address.empty()) {
            const Value value = evaluate_operand_or(state, tid, action.value, 0);
            if (state.memory_model == MemoryModel::TSO) {
                state.store_buffers.at(tid).push_back(StoreBufferEntry{action.address, value});
            } else if (state.memory_model == MemoryModel::PSO) {
                state.pso_store_buffers.at(tid)[action.address].push_back(value);
            } else {
                state.memory_values[action.address] = value;
                report.race = record_write(
                    state,
                    action,
                    MemoryAccess{state.thread_clock.at(tid), endpoint, false, true});
            }
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
        // Lock and woken-Wait reacquire wait on a mutex; rwlock acquisitions
        // distinguish a writer owner from readers that must drain; Join waits
        // on its target thread; sleeping Wait waits on its condition variable
        // without inventing a queued permit; SemWait names its zero-permit
        // semaphore; BarrierWait names the incomplete generation that parked
        // the participant.
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
        } else if (action.kind == ActionKind::RLock ||
                   action.kind == ActionKind::WLock) {
            const RwLockState* rwlock = find_rwlock(state, action.rwlock);
            assert(rwlock != nullptr);
            BlockedThread blocked;
            blocked.thread = tid;
            blocked.rwlock = action.rwlock;
            if (rwlock->writer_holder.has_value()) {
                blocked.kind = BlockedOnKind::RwLockWriter;
                blocked.owner = rwlock->writer_holder;
            } else {
                assert(action.kind == ActionKind::WLock);
                assert(!rwlock->reader_holders.empty());
                blocked.kind = BlockedOnKind::RwLockReaders;
                blocked.self_wait = holds_rwlock_read(*rwlock, tid);
            }
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
        } else if (action.kind == ActionKind::SemWait) {
            BlockedThread blocked;
            blocked.thread = tid;
            blocked.kind = BlockedOnKind::Semaphore;
            blocked.semaphore = action.semaphore;
            report.blocked_threads.push_back(std::move(blocked));
        } else if (action.kind == ActionKind::BarrierWait) {
            const auto barrier = state.barriers.find(action.barrier);
            assert(barrier != state.barriers.end());
            assert(action.parties != 0 &&
                   action.parties == barrier->second.parties);
            assert(barrier->second.arrivals.find(tid) !=
                   barrier->second.arrivals.end());
            (void)barrier;
            BlockedThread blocked;
            blocked.thread = tid;
            blocked.kind = BlockedOnKind::Barrier;
            blocked.barrier = action.barrier;
            report.blocked_threads.push_back(std::move(blocked));
        }
    }
#if defined(DPOR_EXPLORATION_METRICS)
    record_report_schedule_copy(state.schedule);
#endif
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
        [&](const ScheduleStep& step) { return !contains_step(node.sleep, step); });
    if (first_awake == node.enabled.end()) {
        // Sleep sets interact with the max_schedules cutoff only by reducing
        // the number of representative schedules counted. If every enabled
        // transition at this prefix is asleep, the prefix is Mazurkiewicz-
        // equivalent to one already explored and contributes no schedule.
        return;
    }

    insert_step(node.backtrack, *first_awake);

    bool changed = true;
    while (changed) {
        changed = false;
        for (const ScheduleStep& candidate : node.enabled) {
            if (contains_step(node.backtrack, candidate) ||
                contains_step(node.sleep, candidate)) {
                continue;
            }

            for (const ScheduleStep& selected : node.backtrack) {
                const EnabledTransition& candidate_transition =
                    node.enabled_transitions.at(candidate);
                const EnabledTransition& selected_transition =
                    node.enabled_transitions.at(selected);
                const Action& candidate_action =
                    candidate_transition.effective_action;
                const Action& selected_action =
                    selected_transition.effective_action;
                if (candidate.thread == selected.thread &&
                    candidate_action.kind == ActionKind::Flush &&
                    selected_action.kind == ActionKind::Flush &&
                    candidate_action.address != selected_action.address) {
                    // Co-enabled PSO address drains are real scheduler
                    // alternatives even though their direct state effects
                    // commute. Keeping every sibling is the conservative
                    // persistent-set hook that permits another thread to run
                    // between them (the MP discriminator depends on this).
                    insert_step(node.backtrack, candidate);
                    changed = true;
                    break;
                }
                if (candidate_action.kind == ActionKind::BarrierWait &&
                    selected_action.kind == ActionKind::BarrierWait &&
                    candidate_action.barrier == selected_action.barrier) {
                    const auto barrier = state.barriers.find(
                        candidate_action.barrier);
                    if (barrier != state.barriers.end() &&
                        candidate_action.parties != 0 &&
                        candidate_action.parties == barrier->second.parties &&
                        selected_action.parties == barrier->second.parties &&
                        candidate_transition.barrier_generation ==
                            selected_transition.barrier_generation &&
                        candidate_transition.barrier_generation ==
                            std::optional<std::uint64_t>{
                                barrier->second.generation}) {
                        // Early arrivals commute directly, but a persistent
                        // set must still retain every co-enabled sibling so a
                        // later thread may run between them and choose a
                        // different last arriver/generation cohort.
                        insert_step(node.backtrack, candidate);
                        changed = true;
                        break;
                    }
                }
                if (!transitions_independent_at_node(
                        program,
                        state.memory_model,
                        node,
                        candidate.thread,
                        candidate_action,
                        candidate_transition.endpoint,
                        candidate_transition.barrier_generation,
                        selected.thread,
                        selected_action,
                        selected_transition.endpoint,
                        selected_transition.barrier_generation)) {
                    // INVARIANTS.md Soundness/Independence: an enabled
                    // transition is pruned from the initial persistent set
                    // only when the transition predicate says it commutes
                    // with every selected enabled transition. A dependent
                    // enabled transition is kept so a distinct bug class is
                    // not skipped.
                    insert_step(node.backtrack, candidate);
                    changed = true;
                    break;
                }
            }
        }
    }
}

void add_backtracks_for_transition_against_prefix(const Program& program,
                                                  MemoryModel memory_model,
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
#if defined(DPOR_EXPLORATION_METRICS)
        ++profile_metrics().dpor_prefix_entries_scanned;
#endif
        const std::size_t previous_index = index - 1;
        const ExecutedTransition& previous = trace.at(previous_index);
        if (previous.thread == current.thread) {
            // INVARIANTS.md Replay: schedules preserve per-thread action-index
            // order, so same-thread transitions cannot be swapped into a new
            // legal replay schedule.
            continue;
        }

        DporNode& backtrack_point = nodes.at(previous_index);
        if (transitions_independent_at_node(program,
                                            memory_model,
                                            backtrack_point,
                                            previous.thread,
                                            previous.effective_action,
                                            previous.endpoint,
                                            previous.barrier_generation,
                                            current.thread,
                                            current.effective_action,
                                            current.endpoint,
                                            current.barrier_generation)) {
            // INVARIANTS.md Soundness/Independence: this is the DPOR pruning
            // predicate. We may commute and therefore avoid a backtrack only
            // when the transition predicate says ordering cannot affect
            // observable state or future enabledness.
            continue;
        }

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
        insert_step(backtrack_point.backtrack, current.endpoint);
        remove_step(backtrack_point.sleep, current.endpoint);
        return;
    }

    for (const std::size_t disabled_prefix : disabled_dependent_prefixes) {
        DporNode& backtrack_point = nodes.at(disabled_prefix);
        if (const auto repair_steps = disabled_repair_steps(program, backtrack_point, current)) {
            // INVARIANTS.md Soundness/Enabledness: when the disabled
            // transition's concrete enabler chain is known at this prefix,
            // only the first enabled heads of that chain can make the later
            // transition reachable before the dependent earlier transition.
            // Omitted enabled threads do not start the missing thread, finish
            // the join target, or wake the sleeping waiter; independent bug
            // classes involving them remain covered by normal DPOR repairs.
            add_repair_steps(backtrack_point, *repair_steps);
        } else {
            // Conservative fallback for blocked mutex/rwlock acquisitions,
            // zero-permit semaphore waits, incomplete barrier generations,
            // woken reacquires, self-wait cycles, and any chain we cannot
            // compute. The later effective transition could not be scheduled
            // here, so add every enabled thread just as ADR 0010 required.
            add_all_enabled_repair_steps(backtrack_point);
        }
    }
}

void add_backtracks_for_transition(const Program& program,
                                   MemoryModel memory_model,
                                   std::vector<DporNode>& nodes,
                                   const std::vector<ExecutedTransition>& trace) {
    if (trace.empty()) {
        return;
    }

    add_backtracks_for_transition_against_prefix(
        program, memory_model, nodes, trace, trace.back(), trace.size() - 1);
}

void add_disabled_backtracks(const Program& program,
                             const ExecutionState& state,
                             std::vector<DporNode>& nodes,
                             const std::vector<ExecutedTransition>& trace) {
    for (ThreadId tid = 0; tid < program.threads.size(); ++tid) {
        if (is_finished(program, state, tid) ||
            !enabled_steps_for_thread(program, state, tid).empty()) {
            continue;
        }
        if (!has_next_action(program, state, tid)) {
            continue;
        }

        const ExecutedTransition blocked{
            tid,
            effective_next_action(program, state, tid),
            ScheduleStep{tid, state.pc.at(tid), std::nullopt},
            state.thread_clock.at(tid),
            std::nullopt,
            barrier_generation_for(
                state, effective_next_action(program, state, tid)),
        };
        // INVARIANTS.md Soundness/Deadlock: a blocked mutex/rwlock acquisition,
        // zero-permit SemWait, incomplete BarrierWait, Join, sleeping Wait, or
        // woken-Wait reacquire
        // absent from the executed trace may be exactly the dependent action
        // needed to expose another deadlock, race, or error schedule. We
        // therefore apply the same independent()-guarded backtrack rule to
        // disabled next actions at
        // terminal leaves; independent blocked actions remain pruned only when
        // the Independence invariant permits commuting them. effective_next_action()
        // makes the woken-Wait case a mutex reacquire, not a cv wait, while a
        // still-sleeping waiter remains condition-dependent for wakeup order.
        add_backtracks_for_transition_against_prefix(
            program, state.memory_model, nodes, trace, blocked, trace.size());
    }
}

std::vector<ScheduleStep> inherited_sleep_set(const Program& program,
                                              const ExecutionState& state_after_transition,
                                              const DporNode& parent,
                                              const ExecutedTransition& transition) {
    std::vector<ScheduleStep> inherited;
    if (parent.sleep.empty()) {
        return inherited;
    }
    const auto child_transitions = enabled_transitions(program, state_after_transition);
    for (const ScheduleStep& slept : parent.sleep) {
        if (slept == transition.endpoint) {
            continue;
        }
        const auto parent_slept = parent.enabled_transitions.find(slept);
        const auto child = child_transitions.find(slept);
        if (parent_slept == parent.enabled_transitions.end() ||
            child == child_transitions.end() ||
            child->second.effective_action !=
                parent_slept->second.effective_action ||
            child->second.barrier_generation !=
                parent_slept->second.barrier_generation) {
            continue;
        }

        const EnabledTransition& slept_transition = parent_slept->second;
        if (transitions_independent_at_node(
                program,
                state_after_transition.memory_model,
                parent,
                slept.thread,
                slept_transition.effective_action,
                slept_transition.endpoint,
                slept_transition.barrier_generation,
                transition.thread,
                transition.effective_action,
                transition.endpoint,
                transition.barrier_generation)) {
            // Classic Godefroid sleep-set propagation: a slept transition is
            // inherited only when its parent/pre-transition occurrence
            // commutes with the transition just executed and that exact
            // occurrence survives in the child. Barrier arrival independence
            // must use the parent's count: using k+1 after the first arrival
            // would spuriously classify a parties=3 early pair as dependent.
            insert_step(inherited, slept);
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

void record_nontermination(CheckResult& result, const NonTerminationReport& report) {
    ++result.schedules_explored;
    ++result.cycles_detected;
    switch (report.fairness) {
    case Fairness::UnfairScheduleWitness:
        ++result.unfair_cycles;
        break;
    case Fairness::StronglyUnfairScheduleWitness:
        ++result.strongly_unfair_cycles;
        break;
    case Fairness::FairDivergence:
        ++result.fair_cycles;
        break;
    }
    assert(result.fair_cycles + result.strongly_unfair_cycles +
               result.unfair_cycles ==
           result.cycles_detected);
    if (!result.first_nontermination.has_value()) {
        result.first_nontermination = report;
    }
}

NonTerminationReport make_nontermination_report(const Program& program,
                                                 const Schedule& schedule,
                                                 std::size_t cycle_start,
                                                 MemoryModel memory_model);

void dpor_dfs(const Program& program,
              ExecutionState state,
              CheckResult& result,
              std::size_t max_schedules,
              std::size_t step_bound,
              bool detect_cycles,
              std::vector<DporNode>& nodes,
              std::vector<ExecutedTransition>& trace,
              std::vector<ScheduleStep> sleep_set,
              StateHistory& state_history) {
    if (result.schedules_explored >= max_schedules) {
        return;
    }

#if defined(DPOR_EXPLORATION_METRICS)
    ++profile_metrics().dpor_dfs_entries;
    record_dpor_node_snapshot(state);
#endif
    std::vector<ScheduleStep> enabled = enabled_steps(program, state);
    std::map<ScheduleStep, EnabledTransition> transitions =
        enabled_transitions(program, state, enabled);
    const auto depth = nodes.size();
    nodes.push_back(DporNode{
        std::move(enabled),
        std::move(transitions),
        {},
        {},
        std::move(sleep_set),
        state.pc,
        state.started,
        state.mutex_owner,
        state.rwlocks,
        state.wait_phase,
        state.store_buffers,
        state.pso_store_buffers,
        state.barriers,
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
            // only for the enabled transitions it would run. Disabled mutex
            // or rwlock acquisitions, zero-permit semaphore waits, incomplete
            // barrier generations, Join, not-started Spawn targets, and
            // Wait-reacquire transitions can still evidence an earlier
            // enabledness repair, especially before a slept modeled-error
            // endpoint. Apply the terminal disabled fallback before pruning
            // the slept representative.
            add_disabled_backtracks(program, state, nodes, trace);
        }
        nodes.pop_back();
        return;
    }

    while (result.schedules_explored < max_schedules) {
        const std::optional<ScheduleStep> next_step = next_unexplored_backtrack(nodes.at(depth));
        if (!next_step.has_value()) {
            break;
        }

        insert_step(nodes.at(depth).done, *next_step);
        if (!contains_step(nodes.at(depth).enabled, *next_step)) {
            continue;
        }
        if (contains_step(nodes.at(depth).sleep, *next_step)) {
            // Sleep-blocked prefixes are not counted as explored schedules:
            // the schedule budget applies to representatives that actually
            // execute. Choice order remains deterministic because slept
            // backtrack entries are skipped in ascending thread-id order.
            continue;
        }

        if (step_bound_reached(state, next_step->thread, step_bound)) {
            nodes.at(depth).sleep.clear();
            for (const ScheduleStep& enabled : nodes.at(depth).enabled) {
                insert_step(nodes.at(depth).backtrack, enabled);
            }
            record_bound_exceeded(result);
            insert_step(nodes.at(depth).sleep, *next_step);
            continue;
        }

        const EnabledTransition& selected_transition =
            nodes.at(depth).enabled_transitions.at(*next_step);
        const Action effective_action = selected_transition.effective_action;
        const auto barrier_generation =
            selected_transition.barrier_generation;
        bool cycle_cut = false;

#if defined(DPOR_EXPLORATION_METRICS)
        record_branch_state_copy(state);
#endif
        ExecutionState next = state;
        const StepReport step_report = execute_enabled_step(program, next, *next_step);
        const ExecutedTransition transition{
            next_step->thread,
            effective_action,
            *next_step,
            next.thread_clock.at(next_step->thread),
            step_report.spawned_thread,
            barrier_generation,
        };
        trace.push_back(transition);
        add_backtracks_for_transition(program, state.memory_model, nodes, trace);
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
            for (const ScheduleStep& enabled : nodes.at(depth).enabled) {
                insert_step(nodes.at(depth).backtrack, enabled);
            }
            ++result.schedules_explored;
        } else {
#if defined(DPOR_RESTORE_ASSERTS)
            RestoreExactnessReference restore_reference(
                state, state_history, next.schedule);
#endif
            const PathStateObservation observation =
                detect_cycles
                    ? observe_path_behavioral_state(next, state_history)
                    : PathStateObservation{};
            PathHistoryRestore history_restore(state_history, observation.inserted);
            if (observation.cycle_start.has_value()) {
                cycle_cut = true;
                // Cycle-cut outcomes are terminal exploration leaves like
                // step-bound outcomes. Preserve every enabled sibling at the
                // parent so truncating this representative cannot sleep-prune
                // a distinct safety or cycle-existence class.
                nodes.at(depth).sleep.clear();
                for (const ScheduleStep& enabled : nodes.at(depth).enabled) {
                    insert_step(nodes.at(depth).backtrack, enabled);
                }
                record_nontermination(
                    result,
                    make_nontermination_report(
                        program,
                        next.schedule,
                        *observation.cycle_start,
                        next.memory_model));
            } else {
                // INVARIANTS.md Soundness/Independence: enabled transitions not
                // in this node's backtrack set are the only schedules pruned
                // here. The initial persistent set and dynamic backtrack
                // additions above omit an alternative solely after the
                // transition predicate justifies commuting it with the
                // representative transition.
                std::vector<ScheduleStep> child_sleep =
                    inherited_sleep_set(program, next, nodes.at(depth), transition);
                dpor_dfs(program,
                         std::move(next),
                         result,
                         max_schedules,
                         step_bound,
                         detect_cycles,
                         nodes,
                         trace,
                         std::move(child_sleep),
                         state_history);
            }
            history_restore.restore();
#if defined(DPOR_RESTORE_ASSERTS)
            restore_reference.assert_restored(state, state_history);
#endif
        }

        trace.pop_back();
        if (!cycle_cut) {
            insert_step(nodes.at(depth).sleep, *next_step);
        }
    }

    nodes.pop_back();
}

void dfs(const Program& program,
         ExecutionState state,
         CheckResult& result,
         std::size_t max_schedules,
         std::size_t step_bound,
         bool detect_cycles,
         StateHistory& state_history) {
    if (result.schedules_explored >= max_schedules) {
        return;
    }

#if defined(DPOR_EXPLORATION_METRICS)
    ++profile_metrics().naive_dfs_entries;
#endif
    bool explored_child = false;
    for (const ScheduleStep& step : enabled_steps(program, state)) {
        explored_child = true;
        if (step_bound_reached(state, step.thread, step_bound)) {
            record_bound_exceeded(result);
            if (result.schedules_explored >= max_schedules) {
                return;
            }
            continue;
        }

#if defined(DPOR_EXPLORATION_METRICS)
        record_branch_state_copy(state);
#endif
        ExecutionState next = state;
        const StepReport step_report = execute_enabled_step(program, next, step);
        record_step_report(result, step_report);
        if (step_report.error.has_value() || step_report.assertion.has_value()) {
            ++result.schedules_explored;
        } else {
#if defined(DPOR_RESTORE_ASSERTS)
            RestoreExactnessReference restore_reference(
                state, state_history, next.schedule);
#endif
            const PathStateObservation observation =
                detect_cycles
                    ? observe_path_behavioral_state(next, state_history)
                    : PathStateObservation{};
            PathHistoryRestore history_restore(state_history, observation.inserted);
            if (observation.cycle_start.has_value()) {
                record_nontermination(
                    result,
                    make_nontermination_report(
                        program,
                        next.schedule,
                        *observation.cycle_start,
                        next.memory_model));
            } else {
                dfs(program,
                    std::move(next),
                    result,
                    max_schedules,
                    step_bound,
                    detect_cycles,
                    state_history);
            }
            history_restore.restore();
#if defined(DPOR_RESTORE_ASSERTS)
            restore_reference.assert_restored(state, state_history);
#endif
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

void collect_naive_schedules_dfs(const Program& program,
                                 ExecutionState state,
                                 std::vector<Schedule>& schedules,
                                 std::size_t max_schedules,
                                 std::size_t step_bound,
                                 bool detect_cycles,
                                 StateHistory& state_history) {
    if (schedules.size() >= max_schedules) {
        return;
    }

#if defined(DPOR_EXPLORATION_METRICS)
    ++profile_metrics().collector_dfs_entries;
#endif
    bool explored_child = false;
    for (const ScheduleStep& step : enabled_steps(program, state)) {
        explored_child = true;
        if (step_bound_reached(state, step.thread, step_bound)) {
            Schedule bounded = state.schedule;
            bounded.push_back(step);
            schedules.push_back(std::move(bounded));
            if (schedules.size() >= max_schedules) {
                return;
            }
            continue;
        }

#if defined(DPOR_EXPLORATION_METRICS)
        record_branch_state_copy(state, true);
#endif
        ExecutionState next = state;
        const StepReport step_report = execute_enabled_step(program, next, step);
        if (step_report.error.has_value() || step_report.assertion.has_value()) {
            schedules.push_back(next.schedule);
        } else {
#if defined(DPOR_RESTORE_ASSERTS)
            RestoreExactnessReference restore_reference(
                state, state_history, next.schedule);
#endif
            const PathStateObservation observation =
                detect_cycles
                    ? observe_path_behavioral_state(next, state_history)
                    : PathStateObservation{};
            PathHistoryRestore history_restore(state_history, observation.inserted);
            if (observation.cycle_start.has_value()) {
                schedules.push_back(next.schedule);
            } else {
                collect_naive_schedules_dfs(program,
                                            std::move(next),
                                            schedules,
                                            max_schedules,
                                            step_bound,
                                            detect_cycles,
                                            state_history);
            }
            history_restore.restore();
#if defined(DPOR_RESTORE_ASSERTS)
            restore_reference.assert_restored(state, state_history);
#endif
        }

        if (schedules.size() >= max_schedules) {
            return;
        }
    }

    if (!explored_child) {
        schedules.push_back(std::move(state.schedule));
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

    if (is_flush_step(step)) {
        if (state.memory_model == MemoryModel::SC) {
            throw invalid_schedule(step_index, "schedule names a flush step under SC");
        }
        if (state.memory_model == MemoryModel::TSO && step.flush_address.has_value()) {
            throw invalid_schedule(step_index, "TSO flush step includes a PSO address id");
        }
        if (state.memory_model == MemoryModel::PSO && !step.flush_address.has_value()) {
            throw invalid_schedule(step_index, "PSO flush step omits its address id");
        }
    } else {
        if (step.flush_address.has_value()) {
            throw invalid_schedule(step_index, "source action includes a flush address id");
        }
        const auto& thread = program.threads.at(step.thread);
        if (step.action_index >= thread.size()) {
            std::ostringstream reason;
            reason << "schedule names out-of-range action " << step.action_index
                   << " for thread " << step.thread;
            throw invalid_schedule(step_index, reason.str());
        }
    }

    if (!is_flush_step(step) && step.action_index != state.pc.at(step.thread)) {
        std::ostringstream reason;
        reason << "schedule names action " << step.action_index << " for thread " << step.thread
               << " but the next action is " << state.pc.at(step.thread);
        throw invalid_schedule(step_index, reason.str());
    }

    const auto transitions = enabled_transitions(program, state);
    if (transitions.find(step) == transitions.end()) {
        std::ostringstream reason;
        reason << "schedule names a disabled action for thread " << step.thread;
        throw invalid_schedule(step_index, reason.str());
    }
}

NonTerminationReport make_nontermination_report(const Program& program,
                                                 const Schedule& schedule,
                                                 std::size_t cycle_start,
                                                 MemoryModel memory_model) {
    assert(cycle_start < schedule.size());

    std::vector<bool> participant(program.threads.size(), false);
    for (std::size_t index = cycle_start; index < schedule.size(); ++index) {
        participant.at(schedule.at(index).thread) = true;
    }

    ExecutionState state = initial_state(program, memory_model);
    for (std::size_t index = 0; index < cycle_start; ++index) {
        validate_replay_step(program, state, schedule.at(index), index);
        const StepReport step_report = execute_enabled_step(program, state, schedule.at(index));
        if (step_report.error.has_value() || step_report.assertion.has_value()) {
            throw std::logic_error("nontermination stem replay reached a terminal report");
        }
    }

    const StateFingerprint start_fingerprint = behavioral_state_fingerprint(state);
    std::vector<bool> continuously_enabled(program.threads.size(), true);
    std::set<ScheduleStep> enabled_at_least_once;
    const auto observe_enabledness = [&]() {
        for (ThreadId tid = 0; tid < program.threads.size(); ++tid) {
            if (participant.at(tid)) {
                continue;
            }
            const std::vector<ScheduleStep> thread_steps =
                enabled_steps_for_thread(program, state, tid);
            if (thread_steps.empty()) {
                continuously_enabled.at(tid) = false;
            }
            enabled_at_least_once.insert(thread_steps.begin(), thread_steps.end());
        }
    };

    // Preserve ADR 0018's weak predicate exactly: a non-participant thread is
    // continuously enabled when it has some source or flush transition at
    // every cycle state. The cycle repeats forever, so every exact endpoint
    // observed even once is enabled infinitely often for strong fairness.
    // Check the cycle start and every successor state; the closing state
    // intentionally repeats the start and validates exact closure internally.
    observe_enabledness();
    for (std::size_t index = cycle_start; index < schedule.size(); ++index) {
        validate_replay_step(program, state, schedule.at(index), index);
        const StepReport step_report = execute_enabled_step(program, state, schedule.at(index));
        if (step_report.error.has_value() || step_report.assertion.has_value()) {
            throw std::logic_error("nontermination cycle replay reached a terminal report");
        }
        observe_enabledness();
    }
    if (behavioral_state_fingerprint(state) != start_fingerprint) {
        throw std::logic_error("nontermination cycle replay did not close exactly");
    }

    Fairness fairness = enabled_at_least_once.empty()
        ? Fairness::FairDivergence
        : Fairness::StronglyUnfairScheduleWitness;
    for (ThreadId tid = 0; tid < program.threads.size(); ++tid) {
        if (!participant.at(tid) && continuously_enabled.at(tid)) {
            fairness = Fairness::UnfairScheduleWitness;
            break;
        }
    }

    NonTerminationReport report;
    report.fairness = fairness;
    report.stem.assign(schedule.begin(), schedule.begin() + cycle_start);
    report.cycle.assign(schedule.begin() + cycle_start, schedule.end());
    report.schedule = schedule;
#if defined(DPOR_EXPLORATION_METRICS)
    auto& metrics = profile_metrics();
    metrics.report_schedule_copies += 3;
    metrics.report_schedule_steps_copied += schedule.size() * 2;
#endif
    assert(!report.cycle.empty());
    return report;
}

CheckResult replay_schedule(const Program& program,
                            const Schedule& schedule,
                            std::size_t step_bound,
                            MemoryModel memory_model) {
#if defined(DPOR_EXPLORATION_METRICS)
    auto& metrics = profile_metrics();
    ++metrics.replay_calls;
    metrics.replay_steps += schedule.size();
#endif
    CheckResult result;
    ExecutionState state = initial_state(program, memory_model);
    const bool detect_cycles = requires_cycle_detection(program);
    StateHistory state_history =
        detect_cycles ? initial_state_history(state) : StateHistory{};

    for (std::size_t i = 0; i < schedule.size(); ++i) {
        validate_replay_step(program, state, schedule[i], i);
        if (step_bound_reached(state, schedule[i].thread, step_bound)) {
            if (i + 1 < schedule.size()) {
                throw invalid_schedule(i + 1, "schedule continues after a step-bound outcome");
            }
            record_bound_exceeded(result);
            return result;
        }
        const StepReport step_report = execute_enabled_step(program, state, schedule[i]);
        record_step_report(result, step_report);
        if (step_report.error.has_value() || step_report.assertion.has_value()) {
            if (i + 1 < schedule.size()) {
                throw invalid_schedule(i + 1, "schedule continues after a terminal execution report");
            }
            ++result.schedules_explored;
            return result;
        }

        const auto cycle_start =
            detect_cycles
                ? observe_behavioral_state(state, state_history)
                : std::optional<std::size_t>{};
        if (cycle_start.has_value()) {
            if (i + 1 < schedule.size()) {
                throw invalid_schedule(i + 1, "schedule continues after a nontermination cycle");
            }
            record_nontermination(
                result,
                make_nontermination_report(
                    program, state.schedule, *cycle_start, state.memory_model));
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

std::vector<EffectiveScheduleStep> replay_effective_trace(const Program& program,
                                                          const Schedule& schedule,
                                                          std::size_t step_bound,
                                                          MemoryModel memory_model) {
#if defined(DPOR_EXPLORATION_METRICS)
    ++profile_metrics().effective_replay_calls;
#endif
    std::vector<EffectiveScheduleStep> trace;
    ExecutionState state = initial_state(program, memory_model);
    const bool detect_cycles = requires_cycle_detection(program);
    StateHistory state_history =
        detect_cycles ? initial_state_history(state) : StateHistory{};

    for (std::size_t i = 0; i < schedule.size(); ++i) {
        validate_replay_step(program, state, schedule[i], i);
        if (step_bound_reached(state, schedule[i].thread, step_bound)) {
            if (i + 1 < schedule.size()) {
                throw invalid_schedule(i + 1, "schedule continues after a step-bound outcome");
            }
            return trace;
        }

        trace.push_back(EffectiveScheduleStep{
            schedule[i],
            effective_action_for_step(program, state, schedule[i]),
        });

        const StepReport step_report = execute_enabled_step(program, state, schedule[i]);
        if (step_report.error.has_value() || step_report.assertion.has_value()) {
            if (i + 1 < schedule.size()) {
                throw invalid_schedule(i + 1, "schedule continues after a terminal execution report");
            }
            return trace;
        }

        if (detect_cycles &&
            observe_behavioral_state(state, state_history).has_value()) {
            if (i + 1 < schedule.size()) {
                throw invalid_schedule(i + 1, "schedule continues after a nontermination cycle");
            }
            return trace;
        }
    }

    return trace;
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
    if (lhs.rwlock != rhs.rwlock) {
        return lhs.rwlock < rhs.rwlock;
    }
    if (lhs.semaphore != rhs.semaphore) {
        return lhs.semaphore < rhs.semaphore;
    }
    if (lhs.barrier != rhs.barrier) {
        return lhs.barrier < rhs.barrier;
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
    if (lhs.condition != rhs.condition) {
        return lhs.condition < rhs.condition;
    }
    return lhs.self_wait < rhs.self_wait;
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
                                          std::size_t step_bound,
                                          MemoryModel memory_model) {
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

#if defined(DPOR_EXPLORATION_METRICS)
            profile_metrics().minimization_schedule_steps_copied += minimized.size();
#endif
            Schedule candidate = minimized;
            candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(*last_step_index));

            try {
#if defined(DPOR_EXPLORATION_METRICS)
                ++profile_metrics().minimization_candidate_replays;
#endif
                const CheckResult replayed = replay_schedule(program, candidate, step_bound, memory_model);
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
                                 std::size_t step_bound,
                                 MemoryModel memory_model) {
    BugIdentitySet target;
    target.race = identity_of(report);

    const Schedule minimized =
        minimize_schedule_for_identities(program, report.schedule, target, step_bound, memory_model);
    const CheckResult replayed = replay_schedule(program, minimized, step_bound, memory_model);
    if (!reproduces_identities(replayed, target) || !replayed.first_race.has_value()) {
        throw std::logic_error("race schedule minimization failed to preserve bug identity");
    }
    return *replayed.first_race;
}

DeadlockReport minimized_deadlock_report(const Program& program,
                                         const DeadlockReport& report,
                                         std::size_t step_bound,
                                         MemoryModel memory_model) {
    BugIdentitySet target;
    target.deadlock = identity_of(report);

    const Schedule minimized =
        minimize_schedule_for_identities(program, report.schedule, target, step_bound, memory_model);
    const CheckResult replayed = replay_schedule(program, minimized, step_bound, memory_model);
    if (!reproduces_identities(replayed, target) || !replayed.first_deadlock.has_value()) {
        throw std::logic_error("deadlock schedule minimization failed to preserve bug identity");
    }
    return *replayed.first_deadlock;
}

ModelErrorReport minimized_error_report(const Program& program,
                                        const ModelErrorReport& report,
                                        std::size_t step_bound,
                                        MemoryModel memory_model) {
    BugIdentitySet target;
    target.error = identity_of(report);

    const Schedule minimized =
        minimize_schedule_for_identities(program, report.schedule, target, step_bound, memory_model);
    const CheckResult replayed = replay_schedule(program, minimized, step_bound, memory_model);
    if (!reproduces_identities(replayed, target) || !replayed.first_error.has_value()) {
        throw std::logic_error("error schedule minimization failed to preserve bug identity");
    }
    return *replayed.first_error;
}

AssertionFailureReport minimized_assertion_report(const Program& program,
                                                  const AssertionFailureReport& report,
                                                  std::size_t step_bound,
                                                  MemoryModel memory_model) {
    BugIdentitySet target;
    target.assertion = identity_of(report);

    const Schedule minimized =
        minimize_schedule_for_identities(program, report.schedule, target, step_bound, memory_model);
    const CheckResult replayed = replay_schedule(program, minimized, step_bound, memory_model);
    if (!reproduces_identities(replayed, target) || !replayed.first_assertion.has_value()) {
        throw std::logic_error("assertion schedule minimization failed to preserve bug identity");
    }
    return *replayed.first_assertion;
}

void minimize_result_reports(const Program& program,
                             CheckResult& result,
                             std::size_t step_bound,
                             MemoryModel memory_model) {
#if defined(DPOR_EXPLORATION_METRICS)
    ++profile_metrics().minimization_calls;
#endif
    if (result.first_race.has_value()) {
        result.first_race = minimized_race_report(program, *result.first_race, step_bound, memory_model);
    }
    if (result.first_deadlock.has_value()) {
        result.first_deadlock =
            minimized_deadlock_report(program, *result.first_deadlock, step_bound, memory_model);
    }
    if (result.first_error.has_value()) {
        result.first_error = minimized_error_report(program, *result.first_error, step_bound, memory_model);
    }
    if (result.first_assertion.has_value()) {
        result.first_assertion =
            minimized_assertion_report(program, *result.first_assertion, step_bound, memory_model);
    }
}

} // namespace

ModelChecker::ModelChecker(Program program, std::size_t step_bound, MemoryModel memory_model)
    : program_(std::move(program)), step_bound_(step_bound), memory_model_(memory_model) {
    if (step_bound_ == 0) {
        throw std::invalid_argument("step bound must be greater than zero");
    }

    std::set<std::string> mutex_names;
    std::set<std::string> rwlock_names;
    std::set<std::string> semaphore_names;
    std::set<std::string> condition_names;
    std::set<std::string> barrier_names;
    for (const auto& thread : program_.threads) {
        for (const Action& action : thread) {
            switch (action.kind) {
            case ActionKind::Lock:
            case ActionKind::TryLock:
            case ActionKind::Unlock:
                mutex_names.insert(action.mutex);
                break;
            case ActionKind::Wait:
                mutex_names.insert(action.mutex);
                condition_names.insert(action.condition);
                break;
            case ActionKind::Signal:
            case ActionKind::Broadcast:
                condition_names.insert(action.condition);
                break;
            case ActionKind::RLock:
            case ActionKind::RUnlock:
            case ActionKind::WLock:
            case ActionKind::WUnlock:
                rwlock_names.insert(action.rwlock);
                break;
            case ActionKind::SemPost:
            case ActionKind::SemWait:
                semaphore_names.insert(action.semaphore);
                break;
            case ActionKind::BarrierWait:
                barrier_names.insert(action.barrier);
                break;
            default:
                break;
            }
        }
    }
    for (const std::string& name : mutex_names) {
        if (rwlock_names.find(name) != rwlock_names.end()) {
            throw std::invalid_argument(
                "name '" + name + "' cannot be used as both a mutex and rwlock");
        }
    }
    for (const std::string& name : semaphore_names) {
        if (mutex_names.find(name) != mutex_names.end()) {
            throw std::invalid_argument(
                "name '" + name + "' cannot be used as both a mutex and semaphore");
        }
        if (rwlock_names.find(name) != rwlock_names.end()) {
            throw std::invalid_argument(
                "name '" + name + "' cannot be used as both an rwlock and semaphore");
        }
    }
    for (const std::string& name : barrier_names) {
        if (mutex_names.find(name) != mutex_names.end()) {
            throw std::invalid_argument(
                "name '" + name + "' cannot be used as both a barrier and mutex");
        }
        if (rwlock_names.find(name) != rwlock_names.end()) {
            throw std::invalid_argument(
                "name '" + name + "' cannot be used as both a barrier and rwlock");
        }
        if (semaphore_names.find(name) != semaphore_names.end()) {
            throw std::invalid_argument(
                "name '" + name + "' cannot be used as both a barrier and semaphore");
        }
        if (condition_names.find(name) != condition_names.end()) {
            throw std::invalid_argument(
                "name '" + name +
                "' cannot be used as both a barrier and condition variable");
        }
    }
}

CheckResult ModelChecker::explore_naive(std::size_t max_schedules) const {
    CheckResult result;
    ExecutionState state = initial_state(program_, memory_model_);
    const bool detect_cycles = requires_cycle_detection(program_);
    StateHistory state_history =
        detect_cycles ? initial_state_history(state) : StateHistory{};
    dfs(program_,
        std::move(state),
        result,
        max_schedules,
        step_bound_,
        detect_cycles,
        state_history);
    result.exploration_capped = result.schedules_explored >= max_schedules;
    minimize_result_reports(program_, result, step_bound_, memory_model_);
    return result;
}

CheckResult ModelChecker::explore_dpor(std::size_t max_schedules) const {
    CheckResult result;
    std::vector<DporNode> nodes;
    std::vector<ExecutedTransition> trace;
    ExecutionState state = initial_state(program_, memory_model_);
    const bool detect_cycles = requires_cycle_detection(program_);
    StateHistory state_history =
        detect_cycles ? initial_state_history(state) : StateHistory{};
    dpor_dfs(program_,
             std::move(state),
             result,
             max_schedules,
             step_bound_,
             detect_cycles,
             nodes,
             trace,
             {},
             state_history);
    result.exploration_capped = result.schedules_explored >= max_schedules;
    minimize_result_reports(program_, result, step_bound_, memory_model_);
    return result;
}

CheckResult ModelChecker::replay(const Schedule& schedule) const {
    return replay_schedule(program_, schedule, step_bound_, memory_model_);
}

std::vector<Schedule> ModelChecker::collect_naive_schedules(std::size_t max_schedules) const {
    std::vector<Schedule> schedules;
    ExecutionState state = initial_state(program_, memory_model_);
    const bool detect_cycles = requires_cycle_detection(program_);
    StateHistory state_history =
        detect_cycles ? initial_state_history(state) : StateHistory{};
    collect_naive_schedules_dfs(program_,
                                std::move(state),
                                schedules,
                                max_schedules,
                                step_bound_,
                                detect_cycles,
                                state_history);
    return schedules;
}

std::vector<EffectiveScheduleStep> ModelChecker::replay_effective_trace(const Schedule& schedule) const {
    return model::replay_effective_trace(program_, schedule, step_bound_, memory_model_);
}

bool ModelChecker::dpor_transitions_independent(ThreadId lhs_thread,
                                                const Action& lhs,
                                                ThreadId rhs_thread,
                                                const Action& rhs) const {
    return transitions_independent(
        program_, memory_model_, lhs_thread, lhs, rhs_thread, rhs);
}

Schedule ModelChecker::minimize_schedule(const Schedule& schedule) const {
    const CheckResult replayed = replay_schedule(program_, schedule, step_bound_, memory_model_);
    if (replayed.first_nontermination.has_value()) {
        return schedule;
    }
    const BugIdentitySet target = identities_of(replayed);
    if (empty(target)) {
        return schedule;
    }
    return minimize_schedule_for_identities(program_, schedule, target, step_bound_, memory_model_);
}

} // namespace model
