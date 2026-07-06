#include "model/checker.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <utility>

namespace {

model::Action read(std::string address) {
    model::Action action;
    action.kind = model::ActionKind::Read;
    action.address = std::move(address);
    return action;
}

model::Action write(std::string address) {
    model::Action action;
    action.kind = model::ActionKind::Write;
    action.address = std::move(address);
    return action;
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

model::Action join(model::ThreadId target) {
    model::Action action;
    action.kind = model::ActionKind::Join;
    action.target = target;
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

void assert_step(const model::ScheduleStep& step, model::ThreadId thread, std::uint32_t action_index) {
    assert(step.thread == thread);
    assert(step.action_index == action_index);
}

bool has_blocked_cv_waiter(const model::DeadlockReport& report,
                           model::ThreadId thread,
                           const std::string& condition) {
    return std::any_of(report.blocked_threads.begin(), report.blocked_threads.end(), [&](const auto& blocked) {
        return blocked.thread == thread &&
               blocked.kind == model::BlockedOnKind::ConditionVariable &&
               blocked.condition == condition;
    });
}

bool has_blocked_joiner(const model::DeadlockReport& report,
                        model::ThreadId thread,
                        model::ThreadId target) {
    return std::any_of(report.blocked_threads.begin(), report.blocked_threads.end(), [&](const auto& blocked) {
        return blocked.thread == thread &&
               blocked.kind == model::BlockedOnKind::Thread &&
               blocked.target.has_value() &&
               *blocked.target == target;
    });
}

void assert_replays_reports(const model::ModelChecker& checker, const model::CheckResult& result) {
    if (result.first_race.has_value()) {
        const auto replay = checker.replay(result.first_race->schedule);
        assert(replay.first_race.has_value());
        assert(*replay.first_race == *result.first_race);
    }
    if (result.first_deadlock.has_value()) {
        const auto replay = checker.replay(result.first_deadlock->schedule);
        assert(replay.first_deadlock.has_value());
        assert(*replay.first_deadlock == *result.first_deadlock);
    }
    if (result.first_error.has_value()) {
        const auto replay = checker.replay(result.first_error->schedule);
        assert(replay.first_error.has_value());
        assert(*replay.first_error == *result.first_error);
    }
}

void producer_consumer_handoff_replay_completes_cleanly() {
    // With Mesa no-permit semantics, a signaler-first schedule is necessarily
    // the lost-wakeup case tested below. This replay pins the clean handoff
    // path where the waiter sleeps before the signal.
    const model::Program program{{
        {lock("m"), wait("cv", "m"), unlock("m")},
        {lock("m"), write("x"), signal("cv"), unlock("m")},
    }};
    const model::Schedule schedule = {
        {0, 0}, {0, 1},
        {1, 0}, {1, 1}, {1, 2}, {1, 3},
        {0, 1}, {0, 2},
    };

    const model::ModelChecker checker(program);
    const auto replay = checker.replay(schedule);
    assert(replay.schedules_explored == 1);
    assert(!replay.first_race.has_value());
    assert(!replay.first_deadlock.has_value());
    assert(!replay.first_error.has_value());
}

void lost_wakeup_deadlock_is_detected_minimized_and_replayed() {
    const model::Program program{{
        {lock("m"), wait("cv", "m"), unlock("m")},
        {signal("cv")},
    }};

    const model::Schedule lost_wakeup = {
        {1, 0},
        {0, 0}, {0, 1},
    };

    const model::ModelChecker checker(program);
    const auto replay = checker.replay(lost_wakeup);
    assert(replay.first_deadlock.has_value());
    assert(has_blocked_cv_waiter(*replay.first_deadlock, 0, "cv"));

    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    assert(naive.first_deadlock.has_value());
    assert(dpor.first_deadlock.has_value());
    assert(has_blocked_cv_waiter(*naive.first_deadlock, 0, "cv"));
    assert_replays_reports(checker, naive);
    assert_replays_reports(checker, dpor);
}

void broadcast_wakes_multiple_waiters() {
    const model::Program program{{
        {lock("m"), wait("cv", "m"), unlock("m")},
        {lock("m"), wait("cv", "m"), unlock("m")},
        {broadcast("cv")},
    }};
    const model::Schedule schedule = {
        {0, 0}, {0, 1},
        {1, 0}, {1, 1},
        {2, 0},
        {0, 1}, {0, 2},
        {1, 1}, {1, 2},
    };

    const model::ModelChecker checker(program);
    const auto replay = checker.replay(schedule);
    assert(replay.schedules_explored == 1);
    assert(!replay.first_deadlock.has_value());
    assert(!replay.first_error.has_value());
}

void signal_with_no_waiter_does_not_queue_a_permit() {
    const model::Program program{{
        {signal("cv")},
        {lock("m"), wait("cv", "m")},
    }};
    const model::Schedule schedule = {
        {0, 0},
        {1, 0}, {1, 1},
    };

    const model::ModelChecker checker(program);
    const auto replay = checker.replay(schedule);
    assert(replay.first_deadlock.has_value());
    assert(has_blocked_cv_waiter(*replay.first_deadlock, 1, "cv"));
}

void wait_without_mutex_is_modeled_error() {
    const model::Program program{{
        {wait("cv", "m")},
    }};

    const model::ModelChecker checker(program);
    const auto result = checker.explore_naive();
    assert(result.schedules_explored == 1);
    assert(result.first_error.has_value());
    assert(result.first_error->endpoint.thread == 0);
    assert(result.first_error->endpoint.action_index == 0);
    assert(result.first_error->message.find("wait") != std::string::npos);
    assert_replays_reports(checker, result);
}

void join_happens_before_suppresses_race() {
    const model::Program program{{
        {write("x")},
        {join(0), write("x")},
    }};

    const model::ModelChecker checker(program);
    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    assert(!naive.first_race.has_value());
    assert(!dpor.first_race.has_value());
    assert(!naive.first_deadlock.has_value());
    assert(!dpor.first_deadlock.has_value());
    assert(!naive.first_error.has_value());
    assert(!dpor.first_error.has_value());
}

void join_deadlock_cycle_reports_thread_waits() {
    const model::Program program{{
        {join(1)},
        {join(0)},
    }};

    const model::ModelChecker checker(program);
    const auto result = checker.explore_naive();
    assert(result.schedules_explored == 1);
    assert(result.first_deadlock.has_value());
    assert(result.first_deadlock->schedule.empty());
    assert(has_blocked_joiner(*result.first_deadlock, 0, 1));
    assert(has_blocked_joiner(*result.first_deadlock, 1, 0));
    assert_replays_reports(checker, result);
}

void self_join_is_modeled_error() {
    const model::Program program{{
        {join(0)},
    }};

    const model::ModelChecker checker(program);
    const auto result = checker.explore_naive();
    assert(result.schedules_explored == 1);
    assert(result.first_error.has_value());
    assert(result.first_error->endpoint.thread == 0);
    assert(result.first_error->endpoint.action_index == 0);
    assert(result.first_error->message.find("join") != std::string::npos);
    assert_replays_reports(checker, result);
}

void join_of_deadlocked_thread_reports_join_waiter() {
    const model::Program program{{
        {join(1)},
        {join(0)},
        {join(0)},
    }};

    const model::ModelChecker checker(program);
    const auto result = checker.explore_naive();
    assert(result.first_deadlock.has_value());
    assert(has_blocked_joiner(*result.first_deadlock, 2, 0));
    assert_replays_reports(checker, result);
}

void race_through_wait_release_window_is_detected() {
    const model::Program program{{
        {lock("m"), wait("cv", "m"), read("x"), unlock("m")},
        {lock("m"), signal("cv"), unlock("m"), write("x")},
    }};
    const model::Schedule schedule = {
        {0, 0}, {0, 1},
        {1, 0}, {1, 1}, {1, 2},
        {0, 1}, {0, 2},
        {1, 3},
        {0, 3},
    };

    const model::ModelChecker checker(program);
    const auto replay = checker.replay(schedule);
    assert(replay.first_race.has_value());
    assert(replay.first_race->address == "x");

    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    assert(naive.first_race.has_value());
    assert(dpor.first_race.has_value());
    assert(naive.first_race->address == "x");
    assert(dpor.first_race->address == "x");
    assert_replays_reports(checker, naive);
    assert_replays_reports(checker, dpor);
}

} // namespace

int main() {
    producer_consumer_handoff_replay_completes_cleanly();
    lost_wakeup_deadlock_is_detected_minimized_and_replayed();
    broadcast_wakes_multiple_waiters();
    signal_with_no_waiter_does_not_queue_a_permit();
    wait_without_mutex_is_modeled_error();
    join_happens_before_suppresses_race();
    join_deadlock_cycle_reports_thread_waits();
    self_join_is_modeled_error();
    join_of_deadlocked_thread_reports_join_waiter();
    race_through_wait_release_window_is_detected();
    return 0;
}
