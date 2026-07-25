#include "model/checker.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

model::ValueOperand immediate(model::Value value) {
    return model::ValueOperand{
        model::ValueOperandKind::Immediate,
        value,
        0,
    };
}

model::Action lock(std::string mutex) {
    model::Action action;
    action.kind = model::ActionKind::Lock;
    action.mutex = std::move(mutex);
    return action;
}

model::Action unlock(std::string mutex) {
    model::Action action;
    action.kind = model::ActionKind::Unlock;
    action.mutex = std::move(mutex);
    return action;
}

model::Action wait(std::string condition, std::string mutex) {
    model::Action action;
    action.kind = model::ActionKind::Wait;
    action.condition = std::move(condition);
    action.mutex = std::move(mutex);
    return action;
}

model::Action signal(std::string condition) {
    model::Action action;
    action.kind = model::ActionKind::Signal;
    action.condition = std::move(condition);
    return action;
}

model::Action broadcast(std::string condition) {
    model::Action action;
    action.kind = model::ActionKind::Broadcast;
    action.condition = std::move(condition);
    return action;
}

model::Action write(std::string address, model::Value value) {
    model::Action action;
    action.kind = model::ActionKind::Write;
    action.address = std::move(address);
    action.value = immediate(value);
    return action;
}

model::Action atomic_store(std::string address, model::Value value) {
    model::Action action;
    action.kind = model::ActionKind::AtomicStore;
    action.address = std::move(address);
    action.value = immediate(value);
    return action;
}

model::Action atomic_rmw(std::string address,
                         model::Value addend,
                         model::RegisterId destination) {
    model::Action action;
    action.kind = model::ActionKind::AtomicRmw;
    action.address = std::move(address);
    action.value = immediate(addend);
    action.destination = destination;
    return action;
}

model::Action read(std::string address,
                   model::RegisterId destination = 0) {
    model::Action action;
    action.kind = model::ActionKind::Read;
    action.address = std::move(address);
    action.destination = destination;
    return action;
}

model::Action join(model::ThreadId target) {
    model::Action action;
    action.kind = model::ActionKind::Join;
    action.target = target;
    return action;
}

model::Action spawn(model::ThreadId target) {
    model::Action action;
    action.kind = model::ActionKind::Spawn;
    action.target = target;
    return action;
}

model::Action branch_nonzero(model::RegisterId source,
                             std::string label_name) {
    model::Action action;
    action.kind = model::ActionKind::BranchNonzero;
    action.source_register = source;
    action.label = std::move(label_name);
    return action;
}

model::Action assert_nonzero(model::RegisterId source) {
    model::Action action;
    action.kind = model::ActionKind::Assert;
    action.source_register = source;
    return action;
}

model::Action label(std::string name) {
    model::Action action;
    action.kind = model::ActionKind::Label;
    action.label = std::move(name);
    return action;
}

model::Action sem_post(std::string semaphore) {
    model::Action action;
    action.kind = model::ActionKind::SemPost;
    action.semaphore = std::move(semaphore);
    return action;
}

model::Action sem_wait(std::string semaphore) {
    model::Action action;
    action.kind = model::ActionKind::SemWait;
    action.semaphore = std::move(semaphore);
    return action;
}

model::ScheduleStep step(model::ThreadId thread,
                         std::uint32_t action_index) {
    return model::ScheduleStep{thread, action_index, std::nullopt};
}

model::ScheduleStep flush_step(model::ThreadId thread,
                               model::MemoryModel memory_model) {
    return model::ScheduleStep{
        thread,
        model::kFlushActionIndex,
        memory_model == model::MemoryModel::PSO
            ? std::optional<std::uint32_t>{0}
            : std::nullopt,
    };
}

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void require_count(std::size_t actual,
                   std::size_t expected,
                   const char* label_name) {
    if (actual != expected) {
        throw std::runtime_error(
            std::string(label_name) + ": expected " +
            std::to_string(expected) + ", got " + std::to_string(actual));
    }
}

void require_clean(const model::CheckResult& result, const char* message) {
    require(!result.first_race.has_value(), message);
    require(!result.first_deadlock.has_value(), message);
    require(!result.first_error.has_value(), message);
    require(!result.first_assertion.has_value(), message);
    require(!result.first_nontermination.has_value(), message);
    require(result.bound_exceeded_executions == 0, message);
    require(result.cycles_detected == 0, message);
    require(!result.exploration_capped, message);
}

bool has_blocked_cv_waiter(const model::DeadlockReport& report,
                           model::ThreadId thread,
                           const std::string& condition) {
    return std::any_of(
        report.blocked_threads.begin(),
        report.blocked_threads.end(),
        [&](const model::BlockedThread& blocked) {
            return blocked.thread == thread &&
                   blocked.kind ==
                       model::BlockedOnKind::ConditionVariable &&
                   blocked.condition == condition;
        });
}

void require_replays_reports(const model::ModelChecker& checker,
                             const model::CheckResult& result) {
    if (result.first_race.has_value()) {
        const auto replay = checker.replay(result.first_race->schedule);
        require(replay.first_race == result.first_race,
                "explored Broadcast race did not replay identically");
    }
    if (result.first_deadlock.has_value()) {
        const auto replay = checker.replay(result.first_deadlock->schedule);
        require(replay.first_deadlock == result.first_deadlock,
                "explored Broadcast deadlock did not replay identically");
    }
    if (result.first_error.has_value()) {
        const auto replay = checker.replay(result.first_error->schedule);
        require(replay.first_error == result.first_error,
                "explored Broadcast error did not replay identically");
    }
    if (result.first_assertion.has_value()) {
        const auto replay = checker.replay(result.first_assertion->schedule);
        require(replay.first_assertion == result.first_assertion,
                "explored Broadcast assertion did not replay identically");
    }
}

void broadcast_occurrence_identity_includes_the_waking_set() {
    const model::Program one_waiter_program{{
        {broadcast("cv")},
        {lock("m"), wait("cv", "m")},
    }};
    const model::ModelChecker one_waiter_checker(one_waiter_program);

    const auto empty_trace =
        one_waiter_checker.replay_effective_trace({step(0, 0)});
    const auto one_waiter_trace =
        one_waiter_checker.replay_effective_trace(
        model::Schedule{{1, 0}, {1, 1}, {0, 0}});

    require(empty_trace.size() == 1, "empty-Broadcast trace length changed");
    require(one_waiter_trace.size() == 3, "parked-waiter trace length changed");
    require(empty_trace.front().broadcast_waking_set ==
                std::optional<std::vector<model::ThreadId>>{
                    std::vector<model::ThreadId>{}},
            "empty Broadcast did not retain an engaged empty occurrence set");
    require(one_waiter_trace.back().broadcast_waking_set ==
                std::optional<std::vector<model::ThreadId>>{
                    std::vector<model::ThreadId>{1}},
            "one-waiter Broadcast did not retain its exact occurrence set");
    require(
        empty_trace.front() != one_waiter_trace.back(),
        "Broadcast occurrences with different waking sets were conflated");

    const model::Program two_waiter_program{{
        {broadcast("cv")},
        {lock("m1"), wait("cv", "m1")},
        {lock("m2"), wait("cv", "m2")},
    }};
    const auto two_waiter_trace =
        model::ModelChecker(two_waiter_program).replay_effective_trace({
            step(2, 0), step(2, 1),
            step(1, 0), step(1, 1),
            step(0, 0),
        });
    require(two_waiter_trace.back().broadcast_waking_set ==
                std::optional<std::vector<model::ThreadId>>{
                    std::vector<model::ThreadId>{1, 2}},
            "multi-wake Broadcast occurrence set was not sorted and exact");
    require(two_waiter_trace.front().broadcast_waking_set == std::nullopt,
            "non-Broadcast transition acquired Broadcast occurrence identity");
}

void broadcast_wakes_every_parked_waiter_and_empty_broadcast_stores_no_permit() {
    const model::Program fanout{{
        {lock("m"), wait("cv", "m"), unlock("m")},
        {lock("m"), wait("cv", "m"), unlock("m")},
        {broadcast("cv")},
    }};
    require_clean(
        model::ModelChecker(fanout).replay({
            step(0, 0), step(0, 1),
            step(1, 0), step(1, 1),
            step(2, 0),
            step(1, 1), step(1, 2),
            step(0, 1), step(0, 2),
        }),
        "Broadcast did not wake and reacquire every parked waiter");

    const model::Program lost_wakeup{{
        {broadcast("cv")},
        {lock("m"), wait("cv", "m")},
    }};
    const auto replay = model::ModelChecker(lost_wakeup).replay({
        step(0, 0),
        step(1, 0), step(1, 1),
    });
    require(replay.first_deadlock.has_value(),
            "empty Broadcast queued a permit for a future Wait");
    require(has_blocked_cv_waiter(*replay.first_deadlock, 1, "cv"),
            "empty-Broadcast deadlock lost its exact condition blocker");
}

void repeated_source_endpoint_uses_the_exact_broadcast_occurrence() {
    const model::Program program{{
        {
            atomic_store("iterations", 1),
            label("again"),
            broadcast("cv"),
            atomic_rmw("iterations", -1, 0),
            branch_nonzero(0, "again"),
        },
        {lock("m"), wait("cv", "m"), unlock("m")},
    }};
    const model::ModelChecker checker(program);
    const auto trace = checker.replay_effective_trace({
        step(0, 0),
        step(0, 2), step(0, 3), step(0, 4),
        step(1, 0), step(1, 1),
        step(0, 2), step(0, 3), step(0, 4),
        step(1, 1), step(1, 2),
    });

    std::vector<std::vector<model::ThreadId>> waking_sets;
    for (const model::EffectiveScheduleStep& transition : trace) {
        if (transition.endpoint == step(0, 2)) {
            require(transition.broadcast_waking_set.has_value(),
                    "looped Broadcast omitted its occurrence identity");
            waking_sets.push_back(*transition.broadcast_waking_set);
        }
    }
    require(waking_sets ==
                std::vector<std::vector<model::ThreadId>>{{}, {1}},
            "same Broadcast endpoint conflated empty and waking occurrences");

    const model::CheckResult naive = checker.explore_naive();
    const model::CheckResult dpor = checker.explore_dpor();
    require(naive.first_deadlock.has_value() &&
                dpor.first_deadlock.has_value(),
            "looped Broadcast occurrence discriminator lost its deadlock class");
    require(!naive.first_race.has_value() && !dpor.first_race.has_value() &&
                !naive.first_error.has_value() && !dpor.first_error.has_value() &&
                !naive.first_assertion.has_value() &&
                !dpor.first_assertion.has_value() &&
                !naive.first_nontermination.has_value() &&
                !dpor.first_nontermination.has_value() &&
                !naive.exploration_capped && !dpor.exploration_capped &&
                dpor.schedules_explored <= naive.schedules_explored,
            "looped Broadcast occurrence discriminator changed verdict shape");
    require_replays_reports(checker, naive);
    require_replays_reports(checker, dpor);
    require_count(naive.schedules_explored, 156,
                  "looped Broadcast naive schedules");
    require_count(dpor.schedules_explored, 6,
                  "looped Broadcast DPOR schedules");
}

model::Program waking_set_stamp_program(bool broadcaster_is_lowest_thread) {
    const std::vector<model::Action> broadcaster{
        atomic_store("iterations", 1),
        label("again"),
        broadcast("cv"),
        atomic_rmw("iterations", -1, 0),
        branch_nonzero(0, "again"),
    };
    const std::vector<model::Action> waiter{
        lock("m"),
        wait("cv", "m"),
        write("x", 1),
        unlock("m"),
    };
    const std::vector<model::Action> reader{read("x", 1)};
    if (broadcaster_is_lowest_thread) {
        return model::Program{{broadcaster, waiter, reader}};
    }
    return model::Program{{reader, waiter, broadcaster}};
}

void waking_set_stamp_distinguishes_cyclic_backtracks_in_both_directions() {
    struct Direction {
        bool broadcaster_is_lowest_thread;
        std::size_t expected_dpor_schedules;
        const char* count_label;
    };
    const std::vector<Direction> directions{
        {true, 16, "low-to-high waking-set-stamped DPOR schedules"},
        {false, 25, "high-to-low waking-set-stamped DPOR schedules"},
    };

    for (const Direction& direction : directions) {
        const model::Program program =
            waking_set_stamp_program(direction.broadcaster_is_lowest_thread);
        const model::ModelChecker checker(program, 28);
        const model::CheckResult naive = checker.explore_naive();
        const model::CheckResult dpor = checker.explore_dpor();

        require_count(naive.schedules_explored, 3954,
                      "waking-set-stamp naive schedules");
        require_count(dpor.schedules_explored,
                      direction.expected_dpor_schedules,
                      direction.count_label);
        for (const model::CheckResult* result : {&naive, &dpor}) {
            require(result->first_race.has_value() &&
                        result->first_deadlock.has_value() &&
                        !result->first_error.has_value() &&
                        !result->first_assertion.has_value() &&
                        !result->first_nontermination.has_value() &&
                        result->bound_exceeded_executions == 0 &&
                        !result->exploration_capped,
                    "waking-set-stamp discriminator changed verdict shape");
            require_replays_reports(checker, *result);
        }
    }
}

model::Program reacquisition_order_program() {
    return model::Program{{
        {lock("m"), wait("cv", "m"), write("x", 0), unlock("m")},
        {lock("m"), wait("cv", "m"), write("x", 1), unlock("m")},
        {
            broadcast("cv"),
            join(0),
            join(1),
            read("x", 0),
            branch_nonzero(0, "nonzero"),
            assert_nonzero(0),
            label("nonzero"),
            unlock("not_owned"),
        },
    }};
}

void multiwake_reacquisition_orders_preserve_every_verdict_class() {
    const model::Program program = reacquisition_order_program();
    const model::ModelChecker checker(program);

    const auto waiter_one_last = checker.replay({
        step(0, 0), step(0, 1),
        step(1, 0), step(1, 1),
        step(2, 0),
        step(0, 1), step(0, 2), step(0, 3),
        step(1, 1), step(1, 2), step(1, 3),
        step(2, 1), step(2, 2), step(2, 3), step(2, 4),
        step(2, 7),
    });
    require(waiter_one_last.first_error.has_value() &&
                waiter_one_last.first_error->endpoint == step(2, 7),
            "waiter-one-last reacquisition order lost its error verdict");

    const auto waiter_zero_last = checker.replay({
        step(0, 0), step(0, 1),
        step(1, 0), step(1, 1),
        step(2, 0),
        step(1, 1), step(1, 2), step(1, 3),
        step(0, 1), step(0, 2), step(0, 3),
        step(2, 1), step(2, 2), step(2, 3), step(2, 4),
        step(2, 5),
    });
    require(waiter_zero_last.first_assertion.has_value() &&
                waiter_zero_last.first_assertion->endpoint == step(2, 5),
            "waiter-zero-last reacquisition order lost its assertion verdict");

    const model::CheckResult naive = checker.explore_naive();
    const model::CheckResult dpor = checker.explore_dpor();
    for (const model::CheckResult* result : {&naive, &dpor}) {
        require(result->first_deadlock.has_value(),
                "reacquisition discriminator lost its lost-wakeup class");
        require(result->first_error.has_value(),
                "reacquisition discriminator lost its error order class");
        require(result->first_assertion.has_value(),
                "reacquisition discriminator lost its assertion order class");
        require(!result->first_race.has_value() &&
                    !result->first_nontermination.has_value() &&
                    result->bound_exceeded_executions == 0 &&
                    !result->exploration_capped,
                "reacquisition discriminator produced an unexpected class");
        require_replays_reports(checker, *result);
    }
    require_count(naive.schedules_explored, 22,
                  "Broadcast reacquisition naive schedules");
    require_count(dpor.schedules_explored, 14,
                  "Broadcast reacquisition DPOR schedules");
}

model::Program rewait_two_signals_program() {
    std::vector<model::Action> waker = {
        sem_wait("ready"),
        sem_wait("ready"),
        lock("m0"),
        lock("m1"),
        signal("cv"),
        unlock("m0"),
        signal("cv"),
        unlock("m1"),
    };

    return model::Program{{
        {
            lock("gate"),
            lock("m0"),
            sem_post("ready"),
            wait("cv", "m0"),
            wait("cv", "gate"),
            unlock("gate"),
            unlock("m0"),
        },
        {
            lock("m1"),
            sem_post("ready"),
            wait("cv", "m1"),
            lock("gate"),
            signal("cv"),
            unlock("gate"),
            unlock("m1"),
        },
        std::move(waker),
    }};
}

model::Program forced_park_differential_program(bool use_broadcast) {
    std::vector<model::Action> coordinator;
    if (!use_broadcast) {
        coordinator.push_back(signal("cv"));
    }
    coordinator.push_back(spawn(1));
    coordinator.push_back(spawn(2));
    coordinator.push_back(sem_wait("ready"));
    coordinator.push_back(sem_wait("ready"));
    coordinator.push_back(lock("m"));
    coordinator.push_back(unlock("m"));
    coordinator.push_back(use_broadcast ? broadcast("cv") : signal("cv"));

    return model::Program{{
        std::move(coordinator),
        {lock("m"), sem_post("ready"), wait("cv", "m"), unlock("m")},
        {lock("m"), sem_post("ready"), wait("cv", "m"), unlock("m")},
    }};
}

void broadcast_and_two_signals_have_distinct_lost_wakeup_classes() {
    // Stronger direct witness: both waiters are parked for the first Signal,
    // then thread 0 re-waits before the second Signal and consumes both wakes.
    const model::ModelChecker rewait_checker(rewait_two_signals_program());
    const auto rewait_lost = rewait_checker.replay({
        step(0, 0), step(0, 1), step(0, 2), step(0, 3),
        step(1, 0), step(1, 1), step(1, 2),
        step(2, 0), step(2, 1), step(2, 2), step(2, 3),
        step(2, 4), step(2, 5),
        step(0, 3), step(0, 4),
        step(2, 6), step(2, 7),
        step(0, 4), step(0, 5), step(0, 6),
    });
    require(rewait_lost.first_deadlock.has_value() &&
                has_blocked_cv_waiter(*rewait_lost.first_deadlock, 1, "cv"),
            "re-waiting thread did not expose the two-Signal lost wakeup");

    const model::Program broadcast_program =
        forced_park_differential_program(true);
    const model::ModelChecker broadcast_checker(broadcast_program);
    require_clean(
        broadcast_checker.replay({
            step(0, 0), step(0, 1),
            step(1, 0), step(1, 1), step(0, 2), step(1, 2),
            step(2, 0), step(2, 1), step(0, 3), step(2, 2),
            step(0, 4), step(0, 5), step(0, 6),
            step(1, 2), step(1, 3), step(2, 2), step(2, 3),
        }),
        "single Broadcast did not complete the forced two-waiter protocol");
    const model::CheckResult broadcast_naive =
        broadcast_checker.explore_naive();
    const model::CheckResult broadcast_dpor =
        broadcast_checker.explore_dpor();
    require_clean(broadcast_naive,
                  "Broadcast differential was not clean under naive");
    require_clean(broadcast_dpor,
                  "Broadcast differential was not clean under DPOR");
    require_count(broadcast_naive.schedules_explored, 86,
                  "Broadcast differential naive schedules");
    require_count(broadcast_dpor.schedules_explored, 30,
                  "Broadcast differential DPOR schedules");

    const model::Program signals_program =
        forced_park_differential_program(false);
    const model::ModelChecker signals_checker(signals_program);
    const auto lost_replay = signals_checker.replay({
        step(0, 0), step(0, 1), step(0, 2),
        step(1, 0), step(1, 1), step(0, 3), step(1, 2),
        step(2, 0), step(2, 1), step(0, 4), step(2, 2),
        step(0, 5), step(0, 6), step(0, 7),
        step(1, 2), step(1, 3),
    });
    require(lost_replay.first_deadlock.has_value(),
            "two-Signal variant lost its deadlock witness");
    require(has_blocked_cv_waiter(*lost_replay.first_deadlock, 2, "cv"),
            "two-Signal variant deadlocked the wrong waiter");

    const model::CheckResult signals_naive =
        signals_checker.explore_naive();
    const model::CheckResult signals_dpor =
        signals_checker.explore_dpor();
    for (const model::CheckResult* result :
         {&signals_naive, &signals_dpor}) {
        require(result->first_deadlock.has_value(),
                "two-Signal differential lost its deadlock class");
        require(has_blocked_cv_waiter(*result->first_deadlock, 2, "cv"),
                "two-Signal exploration reported the wrong condition blocker");
        require(!result->first_race.has_value() &&
                    !result->first_error.has_value() &&
                    !result->first_assertion.has_value() &&
                    !result->first_nontermination.has_value() &&
                    result->bound_exceeded_executions == 0 &&
                    !result->exploration_capped,
                "two-Signal differential produced an unexpected bug class");
        require_replays_reports(signals_checker, *result);
    }
    require_count(signals_naive.schedules_explored, 43,
                  "two-Signal differential naive schedules");
    require_count(signals_dpor.schedules_explored, 15,
                  "two-Signal differential DPOR schedules");
}

void buffered_broadcast_requires_an_explicit_drain() {
    const model::Program program{{
        {write("x", 1), broadcast("cv")},
        {lock("m"), wait("cv", "m"), read("x"), unlock("m")},
    }};
    for (const model::MemoryModel memory_model :
         {model::MemoryModel::TSO, model::MemoryModel::PSO}) {
        const model::ModelChecker checker(program, 20, memory_model);
        bool rejected_before_drain = false;
        try {
            (void)checker.replay({
                step(1, 0), step(1, 1),
                step(0, 0), step(0, 1),
            });
        } catch (const std::invalid_argument&) {
            rejected_before_drain = true;
        }
        require(rejected_before_drain,
                "Broadcast executed with a pending store buffer");

        require_clean(
            checker.replay({
                step(1, 0), step(1, 1),
                step(0, 0), flush_step(0, memory_model), step(0, 1),
                step(1, 1), step(1, 2), step(1, 3),
            }),
            "Broadcast did not execute cleanly after an explicit buffer drain");
    }
}

} // namespace

int main() {
    broadcast_occurrence_identity_includes_the_waking_set();
    broadcast_wakes_every_parked_waiter_and_empty_broadcast_stores_no_permit();
    repeated_source_endpoint_uses_the_exact_broadcast_occurrence();
    waking_set_stamp_distinguishes_cyclic_backtracks_in_both_directions();
    multiwake_reacquisition_orders_preserve_every_verdict_class();
    broadcast_and_two_signals_have_distinct_lost_wakeup_classes();
    buffered_broadcast_requires_an_explicit_drain();
    return 0;
}
