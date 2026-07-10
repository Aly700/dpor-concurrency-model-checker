#pragma once

#include "model/action.hpp"

#include <optional>
#include <string>
#include <vector>

namespace model {

enum class MemoryModel { SC, TSO };

struct RaceReport {
    std::string address;
    ScheduleStep first;
    ScheduleStep second;
    Schedule schedule;

    bool operator==(const RaceReport&) const = default;
};

enum class BlockedOnKind { Mutex, Thread, ConditionVariable };

struct BlockedThread {
    ThreadId thread{0};
    std::string mutex;
    std::optional<ThreadId> owner;
    BlockedOnKind kind{BlockedOnKind::Mutex};
    std::optional<ThreadId> target;
    std::string condition;

    bool operator==(const BlockedThread&) const = default;
};

struct DeadlockReport {
    std::vector<BlockedThread> blocked_threads;
    Schedule schedule;

    bool operator==(const DeadlockReport&) const = default;
};

struct ModelErrorReport {
    ScheduleStep endpoint;
    std::string message;
    Schedule schedule;

    bool operator==(const ModelErrorReport&) const = default;
};

struct AssertionFailureReport {
    ScheduleStep endpoint;
    RegisterId reg{0};
    Value value{0};
    Schedule schedule;

    bool operator==(const AssertionFailureReport&) const = default;
};

struct CheckResult {
    std::size_t schedules_explored{0};
    std::size_t bound_exceeded_executions{0};
    // True when exploration stopped at the max_schedules cap: the verdict may
    // be incomplete because unexplored schedules remain (or the space finished
    // exactly at the cap, which cannot be distinguished cheaply — the flag
    // errs toward reporting possible incompleteness, never toward hiding it).
    bool exploration_capped{false};
    std::optional<RaceReport> first_race;
    std::optional<DeadlockReport> first_deadlock;
    std::optional<ModelErrorReport> first_error;
    std::optional<AssertionFailureReport> first_assertion;
};

struct EffectiveScheduleStep {
    ScheduleStep endpoint;
    Action effective_action;

    bool operator==(const EffectiveScheduleStep&) const = default;
};

class ModelChecker {
public:
    static constexpr std::size_t kDefaultStepBound = 2000;

    explicit ModelChecker(Program program,
                          std::size_t step_bound = kDefaultStepBound,
                          MemoryModel memory_model = MemoryModel::SC);
    CheckResult explore_naive(std::size_t max_schedules = 100000) const;
    CheckResult explore_dpor(std::size_t max_schedules = 100000) const;
    CheckResult replay(const Schedule& schedule) const;

    // Verification-meter helpers. These are read-only observations of the
    // existing interpreter and DPOR transition predicate; they do not alter
    // exploration state or schedule choice.
    std::vector<Schedule> collect_naive_schedules(std::size_t max_schedules = 100000) const;
    std::vector<EffectiveScheduleStep> replay_effective_trace(const Schedule& schedule) const;
    bool dpor_transitions_independent(ThreadId lhs_thread,
                                      const Action& lhs,
                                      ThreadId rhs_thread,
                                      const Action& rhs) const;

    // Bug identity for minimization:
    // - race: same modeled address and same unordered pair of
    //   (thread, action_index) endpoints;
    // - deadlock: same set of BlockedThread entries;
    // - modeled error: same (thread, action_index) endpoint.
    //
    // minimize_schedule first replays schedule. If replay reports no race,
    // deadlock, or modeled error, the input is returned unchanged. If replay
    // rejects the input schedule, the replay exception is propagated.
    //
    // Otherwise the result is found by deterministic greedy fixed-point
    // deletion: for each thread in ascending id order, try removing the last
    // remaining step from that thread's subsequence, except race/error
    // endpoint steps, and keep the removal only when an actual replay still
    // reproduces the same bug identity. This protects the replay invariant and
    // failure-output reproducibility, but it is intentionally only 1-minimal
    // with respect to that deletion operator, not globally minimal.
    Schedule minimize_schedule(const Schedule& schedule) const;

private:
    Program program_;
    std::size_t step_bound_{kDefaultStepBound};
    MemoryModel memory_model_{MemoryModel::SC};
};

} // namespace model
