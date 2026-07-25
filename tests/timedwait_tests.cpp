#include "model/checker.hpp"

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

model::ValueOperand reg(model::RegisterId source) {
    model::ValueOperand operand;
    operand.kind = model::ValueOperandKind::Register;
    operand.reg = source;
    return operand;
}

model::Action lock(std::string mutex) {
    model::Action action;
    action.kind = model::ActionKind::Lock;
    action.mutex = std::move(mutex);
    return action;
}

model::Action set(model::RegisterId destination, model::Value value) {
    model::Action action;
    action.kind = model::ActionKind::Set;
    action.destination = destination;
    action.value = immediate(value);
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

model::Action timed_wait(std::string condition,
                         std::string mutex,
                         model::RegisterId destination) {
    model::Action action;
    action.kind = model::ActionKind::TimedWait;
    action.condition = std::move(condition);
    action.mutex = std::move(mutex);
    action.destination = destination;
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

model::Action assert_nonzero(model::RegisterId source) {
    model::Action action;
    action.kind = model::ActionKind::Assert;
    action.source_register = source;
    return action;
}

model::Action branch_nonzero(model::RegisterId source, std::string target) {
    model::Action action;
    action.kind = model::ActionKind::BranchNonzero;
    action.source_register = source;
    action.label = std::move(target);
    return action;
}

model::Action label(std::string name) {
    model::Action action;
    action.kind = model::ActionKind::Label;
    action.label = std::move(name);
    return action;
}

model::Action spawn(model::ThreadId target) {
    model::Action action;
    action.kind = model::ActionKind::Spawn;
    action.target = target;
    return action;
}

model::Action write(std::string address, model::Value value) {
    model::Action action;
    action.kind = model::ActionKind::Write;
    action.address = std::move(address);
    action.value = immediate(value);
    return action;
}

model::Action read(std::string address, model::RegisterId destination) {
    model::Action action;
    action.kind = model::ActionKind::Read;
    action.address = std::move(address);
    action.destination = destination;
    return action;
}

model::Action atomic_store(std::string address, model::Value value) {
    model::Action action;
    action.kind = model::ActionKind::AtomicStore;
    action.address = std::move(address);
    action.value = immediate(value);
    return action;
}

model::Action compare_exchange(std::string address,
                               model::ValueOperand expected,
                               model::ValueOperand desired,
                               model::RegisterId destination) {
    model::Action action;
    action.kind = model::ActionKind::CompareExchange;
    action.address = std::move(address);
    action.expected = expected;
    action.value = desired;
    action.destination = destination;
    return action;
}

model::Action atomic_rmw(std::string address,
                         model::Value delta,
                         model::RegisterId destination) {
    model::Action action;
    action.kind = model::ActionKind::AtomicRmw;
    action.address = std::move(address);
    action.value = immediate(delta);
    action.destination = destination;
    return action;
}

model::ScheduleStep step(model::ThreadId thread, std::uint32_t action_index) {
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

void require_clean(const model::CheckResult& result, const char* message) {
    require(!result.first_race.has_value(), message);
    require(!result.first_deadlock.has_value(), message);
    require(!result.first_error.has_value(), message);
    require(!result.first_assertion.has_value(), message);
    require(!result.first_nontermination.has_value(), message);
    require(result.bound_exceeded_executions == 0, message);
    require(!result.exploration_capped, message);
}

void require_replays_reports(const model::ModelChecker& checker,
                             const model::CheckResult& result) {
    if (result.first_deadlock.has_value()) {
        require(checker.replay(result.first_deadlock->schedule).first_deadlock ==
                    result.first_deadlock,
                "TimedWait deadlock report did not replay identically");
    }
    if (result.first_error.has_value()) {
        require(checker.replay(result.first_error->schedule).first_error ==
                    result.first_error,
                "TimedWait error report did not replay identically");
    }
    if (result.first_assertion.has_value()) {
        require(checker.replay(result.first_assertion->schedule).first_assertion ==
                    result.first_assertion,
                "TimedWait assertion report did not replay identically");
    }
}

void require_timed_occurrence(const model::EffectiveScheduleStep& transition,
                              model::TimedWaitTransition expected_transition,
                              std::uint64_t expected_episode,
                              const char* message) {
    require(transition.timed_wait_occurrence.has_value(), message);
    require(transition.timed_wait_occurrence->transition == expected_transition,
            message);
    require(transition.timed_wait_occurrence->episode == expected_episode,
            message);
}

void timeout_is_an_explicit_three_phase_replay_and_writes_zero() {
    const model::Program program{{{
        lock("m"),
        timed_wait("cv", "m", 0),
        unlock("m"),
    }}};
    const model::ModelChecker checker(program);
    const model::Schedule timeout{
        step(0, 0),
        step(0, 1),
        step(0, 1),
        step(0, 1),
        step(0, 2),
    };

    require_clean(checker.replay(timeout),
                  "a lone TimedWait did not time out, reacquire, and finish cleanly");
    const auto trace = checker.replay_effective_trace(timeout);
    require(trace.size() == timeout.size(), "TimedWait effective trace length changed");
    require_timed_occurrence(trace.at(1),
                             model::TimedWaitTransition::Park,
                             2,
                             "TimedWait park omitted its exact episode");
    require_timed_occurrence(trace.at(2),
                             model::TimedWaitTransition::Timeout,
                             2,
                             "TimedWait timeout omitted its exact parked episode");
    require(trace.at(3).effective_action.kind == model::ActionKind::Lock,
            "TimedWait reacquire was not reduced as an effective Lock");
    require(!trace.at(3).timed_wait_occurrence.has_value(),
            "TimedWait reacquire acquired a park/timeout occurrence stamp");

    const model::Program zero_result{{{
        set(0, 1),
        lock("m"),
        timed_wait("cv", "m", 0),
        assert_nonzero(0),
    }}};
    const model::CheckResult zero_replay =
        model::ModelChecker(zero_result).replay(
            {step(0, 0), step(0, 1),
             step(0, 2), step(0, 2), step(0, 2),
             step(0, 3)});
    require(zero_replay.first_assertion.has_value(),
            "TimedWait timeout did not overwrite a nonzero result with zero");
    require(zero_replay.first_assertion->reg == 0 &&
                zero_replay.first_assertion->value == 0,
            "TimedWait timeout did not write exactly zero");
}

void signal_and_broadcast_wakes_write_one() {
    const model::Program signal_program{{
        {
            atomic_store("signal-exact-one", 1),
            lock("m"),
            timed_wait("cv", "m", 3),
            compare_exchange(
                "signal-exact-one", reg(3), immediate(1), 4),
            assert_nonzero(4),
            unlock("m"),
        },
        {signal("cv")},
    }};
    require_clean(
        model::ModelChecker(signal_program).replay(
            {step(0, 0), step(0, 1), step(0, 2), step(1, 0),
             step(0, 2), step(0, 3), step(0, 4), step(0, 5)}),
        "Signal wake did not write TimedWait result exactly one");

    const model::Program broadcast_program{{
        {
            atomic_store("broadcast-exact-one-0", 1),
            lock("m0"),
            timed_wait("cv", "m0", 0),
            compare_exchange(
                "broadcast-exact-one-0", reg(0), immediate(1), 2),
            assert_nonzero(2),
            unlock("m0"),
        },
        {
            atomic_store("broadcast-exact-one-1", 1),
            lock("m1"),
            timed_wait("cv", "m1", 1),
            compare_exchange(
                "broadcast-exact-one-1", reg(1), immediate(1), 2),
            assert_nonzero(2),
            unlock("m1"),
        },
        {broadcast("cv")},
    }};
    require_clean(
        model::ModelChecker(broadcast_program).replay(
            {step(0, 0), step(0, 1), step(0, 2),
             step(1, 0), step(1, 1), step(1, 2),
             step(2, 0),
             step(0, 2), step(0, 3), step(0, 4), step(0, 5),
             step(1, 2), step(1, 3), step(1, 4), step(1, 5)}),
        "Broadcast did not write result exactly one to every TimedWait it woke");
}

void wake_and_timeout_winner_classes_survive_dpor() {
    const model::Program program{{
        {
            lock("m"),
            spawn(1),
            timed_wait("cv", "m", 0),
            branch_nonzero(0, "woke"),
            assert_nonzero(0),
            label("woke"),
            unlock("never-owned"),
        },
        {
            lock("m"),
            signal("cv"),
            unlock("m"),
        },
    }};
    const model::ModelChecker checker(program);
    const model::CheckResult naive = checker.explore_naive();
    const model::CheckResult dpor = checker.explore_dpor();

    for (const model::CheckResult* result : {&naive, &dpor}) {
        require(result->first_assertion.has_value(),
                "TimedWait timeout result-zero assertion class was pruned");
        require(result->first_error.has_value(),
                "TimedWait wake result-one modeled-error class was pruned");
        require(result->first_assertion->reg == 0 &&
                    result->first_assertion->value == 0,
                "TimedWait timeout class did not expose result zero");
        require(result->first_error->endpoint == step(0, 6),
                "TimedWait wake class did not branch on result one");
        require(!result->first_race.has_value() &&
                    !result->first_deadlock.has_value() &&
                    !result->first_nontermination.has_value() &&
                    result->bound_exceeded_executions == 0 &&
                    !result->exploration_capped,
                "TimedWait winner discriminator produced an unexpected class");
        require_replays_reports(checker, *result);
    }
    require(dpor.schedules_explored <= naive.schedules_explored,
            "TimedWait DPOR explored more winner schedules than naive");
    require(naive.schedules_explored == 4,
            "TimedWait winner discriminator naive schedule count changed");
    require(dpor.schedules_explored == 4,
            "TimedWait winner discriminator DPOR class count changed");
}

void lost_signal_times_out_while_plain_wait_deadlocks() {
    const model::Program timed_program{{
        {signal("cv"), spawn(1)},
        {lock("m"), timed_wait("cv", "m", 0), unlock("m")},
    }};
    const model::Schedule timed_schedule{
        step(0, 0),
        step(0, 1),
        step(1, 0),
        step(1, 1),
        step(1, 1),
        step(1, 1),
        step(1, 2),
    };
    const model::ModelChecker timed_checker(timed_program);
    require_clean(timed_checker.replay(timed_schedule),
                  "a lost wake left a TimedWait deadlocked");
    const model::CheckResult timed_naive = timed_checker.explore_naive();
    const model::CheckResult timed_dpor = timed_checker.explore_dpor();
    require_clean(timed_naive,
                  "lost-wakeup TimedWait was not clean under naive exploration");
    require_clean(timed_dpor,
                  "lost-wakeup TimedWait was not clean under DPOR exploration");
    require(timed_naive.schedules_explored == 1 &&
                timed_dpor.schedules_explored == 1,
            "lost-wakeup TimedWait changed its single timeout class");

    const model::Program plain_program{{
        {signal("cv"), spawn(1)},
        {lock("m"), wait("cv", "m"), unlock("m")},
    }};
    const model::ModelChecker plain_checker(plain_program);
    const model::CheckResult plain_replay = plain_checker.replay(
        {step(0, 0), step(0, 1), step(1, 0), step(1, 1)});
    require(plain_replay.first_deadlock.has_value(),
            "plain Wait lost-wakeup control stopped deadlocking");
    const model::CheckResult plain_naive = plain_checker.explore_naive();
    const model::CheckResult plain_dpor = plain_checker.explore_dpor();
    require(plain_naive.first_deadlock.has_value() &&
                plain_dpor.first_deadlock.has_value() &&
                plain_naive.schedules_explored == 1 &&
                plain_dpor.schedules_explored == 1,
            "plain Wait lost-wakeup control changed its one deadlock class");
    require_replays_reports(plain_checker, plain_naive);
    require_replays_reports(plain_checker, plain_dpor);
}

void timeout_then_blocked_reacquire_is_a_mutex_deadlock() {
    const model::Program program{{
        {lock("m"), timed_wait("cv", "m", 0), unlock("m")},
        {lock("m")},
    }};
    const model::CheckResult replay =
        model::ModelChecker(program).replay(
            {step(0, 0), step(0, 1), step(1, 0), step(0, 1)});
    require(replay.first_deadlock.has_value(),
            "blocked TimedWait reacquisition did not report a deadlock");
    require(replay.first_deadlock->blocked_threads.size() == 1,
            "blocked TimedWait reacquisition changed blocker count");
    const model::BlockedThread& blocked =
        replay.first_deadlock->blocked_threads.front();
    require(blocked.thread == 0 &&
                blocked.kind == model::BlockedOnKind::Mutex &&
                blocked.mutex == "m" &&
                blocked.owner == std::optional<model::ThreadId>{1},
            "TimedWait reacquisition was reported as a condition wait");
}

void timed_wait_is_conservatively_dependent_with_same_condition_wakes() {
    const model::Action timeout = timed_wait("cv", "m", 0);
    const model::Action same_cv_signal = signal("cv");
    const model::Action same_cv_broadcast = broadcast("cv");
    require(!model::independent(timeout, same_cv_signal) &&
                !model::independent(same_cv_signal, timeout),
            "TimedWait/Signal on one condition was classified independent");
    require(!model::independent(timeout, same_cv_broadcast) &&
                !model::independent(same_cv_broadcast, timeout),
            "TimedWait/Broadcast on one condition was classified independent");
    require(!model::independent(timeout, timed_wait("cv", "other", 1)),
            "same-condition TimedWait occurrences were classified independent");
}

void repeated_endpoint_trace_names_each_parked_episode() {
    const model::Program program{{{
        atomic_store("iterations", 1),
        lock("m"),
        label("again"),
        timed_wait("cv", "m", 0),
        atomic_rmw("iterations", -1, 1),
        branch_nonzero(1, "again"),
        unlock("m"),
    }}};
    const model::Schedule schedule{
        step(0, 0),
        step(0, 1),
        step(0, 3), step(0, 3), step(0, 3),
        step(0, 4), step(0, 5),
        step(0, 3), step(0, 3), step(0, 3),
        step(0, 4), step(0, 5), step(0, 6),
    };
    const std::vector<model::EffectiveScheduleStep> trace =
        model::ModelChecker(program, 20).replay_effective_trace(schedule);
    std::vector<model::TimedWaitOccurrence> occurrences;
    for (const model::EffectiveScheduleStep& transition : trace) {
        if (transition.timed_wait_occurrence.has_value()) {
            occurrences.push_back(*transition.timed_wait_occurrence);
        }
    }
    require(occurrences ==
                (std::vector<model::TimedWaitOccurrence>{
                    {model::TimedWaitTransition::Park, 3},
                    {model::TimedWaitTransition::Timeout, 3},
                    {model::TimedWaitTransition::Park, 8},
                    {model::TimedWaitTransition::Timeout, 8},
                }),
            "repeated TimedWait endpoint conflated two parked episodes");
}

model::Program timeout_episode_stamp_program(bool waiter_is_low) {
    const std::vector<model::Action> waiter{
        atomic_store("iterations", 1),
        lock("m"),
        label("again"),
        timed_wait("cv", "m", 0),
        atomic_rmw("iterations", -1, 1),
        branch_nonzero(1, "again"),
        write("x", 1),
        branch_nonzero(0, "woke"),
        assert_nonzero(0),
        label("woke"),
        unlock("never-owned"),
    };
    const std::vector<model::Action> signaler{signal("cv")};
    const std::vector<model::Action> reader{read("x", 2)};
    return waiter_is_low
               ? model::Program{{waiter, signaler, reader}}
               : model::Program{{reader, signaler, waiter}};
}

void timeout_occurrence_stamp_distinguishes_backtracks_in_both_directions() {
    struct Direction {
        bool waiter_is_low;
        std::size_t expected_dpor_schedules;
        const char* count_message;
    };
    const std::vector<Direction> directions{
        {true, 22, "low-to-high TimedWait-stamped DPOR schedules changed"},
        {false, 38, "high-to-low TimedWait-stamped DPOR schedules changed"},
    };
    for (const Direction& direction : directions) {
        const model::Program program =
            timeout_episode_stamp_program(direction.waiter_is_low);
        const model::ModelChecker checker(program, 32);
        const model::CheckResult naive = checker.explore_naive();
        const model::CheckResult dpor = checker.explore_dpor();
        require(naive.schedules_explored == 269,
                "TimedWait occurrence-stamp naive schedules changed");
        require(dpor.schedules_explored == direction.expected_dpor_schedules,
                direction.count_message);
        for (const model::CheckResult* result : {&naive, &dpor}) {
            require(result->first_race.has_value() &&
                        result->first_error.has_value() &&
                        result->first_assertion.has_value() &&
                        !result->first_deadlock.has_value() &&
                        !result->first_nontermination.has_value() &&
                        result->bound_exceeded_executions == 0 &&
                        !result->exploration_capped,
                    "TimedWait occurrence-stamp fixture changed verdict shape");
            require_replays_reports(checker, *result);
        }
    }
}

void buffered_timed_wait_requires_explicit_drains() {
    const model::Program timeout_program{{{
        lock("m"),
        write("x", 1),
        timed_wait("cv", "m", 0),
        unlock("m"),
    }}};
    for (const model::MemoryModel memory_model :
         {model::MemoryModel::TSO, model::MemoryModel::PSO}) {
        const model::ModelChecker checker(timeout_program, 20, memory_model);
        bool rejected_before_drain = false;
        try {
            (void)checker.replay(
                {step(0, 0), step(0, 1), step(0, 2)});
        } catch (const std::invalid_argument&) {
            rejected_before_drain = true;
        }
        require(rejected_before_drain,
                "TimedWait parked with a pending store buffer");
        require_clean(
            checker.replay(
                {step(0, 0), step(0, 1), flush_step(0, memory_model),
                 step(0, 2), step(0, 2), step(0, 2), step(0, 3)}),
            "TimedWait timeout failed after an explicit buffer drain");
    }

    const model::Program wake_program{{
        {
            lock("m"),
            timed_wait("cv", "m", 0),
            read("x", 1),
            unlock("m"),
        },
        {write("x", 1), signal("cv")},
    }};
    for (const model::MemoryModel memory_model :
         {model::MemoryModel::TSO, model::MemoryModel::PSO}) {
        const model::ModelChecker checker(wake_program, 20, memory_model);
        bool rejected_before_drain = false;
        try {
            (void)checker.replay(
                {step(0, 0), step(0, 1), step(1, 0), step(1, 1)});
        } catch (const std::invalid_argument&) {
            rejected_before_drain = true;
        }
        require(rejected_before_drain,
                "Signal woke TimedWait with a pending signaler buffer");
        require_clean(
            checker.replay(
                {step(0, 0), step(0, 1), step(1, 0),
                 flush_step(1, memory_model), step(1, 1),
                 step(0, 1), step(0, 2), step(0, 3)}),
            "buffered Signal did not publish to TimedWait after draining");
    }
}

} // namespace

int main() {
    repeated_endpoint_trace_names_each_parked_episode();
    timeout_occurrence_stamp_distinguishes_backtracks_in_both_directions();
    timeout_is_an_explicit_three_phase_replay_and_writes_zero();
    signal_and_broadcast_wakes_write_one();
    wake_and_timeout_winner_classes_survive_dpor();
    lost_signal_times_out_while_plain_wait_deadlocks();
    timeout_then_blocked_reacquire_is_a_mutex_deadlock();
    timed_wait_is_conservatively_dependent_with_same_condition_wakes();
    buffered_timed_wait_requires_explicit_drains();
    return 0;
}
