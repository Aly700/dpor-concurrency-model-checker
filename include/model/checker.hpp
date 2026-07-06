#pragma once

#include "model/action.hpp"

#include <optional>
#include <string>
#include <vector>

namespace model {

struct RaceReport {
    std::string address;
    ScheduleStep first;
    ScheduleStep second;
    Schedule schedule;

    bool operator==(const RaceReport&) const = default;
};

struct BlockedThread {
    ThreadId thread{0};
    std::string mutex;
    std::optional<ThreadId> owner;

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

struct CheckResult {
    std::size_t schedules_explored{0};
    std::optional<RaceReport> first_race;
    std::optional<DeadlockReport> first_deadlock;
    std::optional<ModelErrorReport> first_error;
};

class ModelChecker {
public:
    explicit ModelChecker(Program program);
    CheckResult explore_naive(std::size_t max_schedules = 100000) const;
    CheckResult explore_dpor(std::size_t max_schedules = 100000) const;
    CheckResult replay(const Schedule& schedule) const;

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
};

} // namespace model
