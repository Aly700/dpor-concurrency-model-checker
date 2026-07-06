#include "model/checker.hpp"

#include <cassert>
#include <stdexcept>
#include <string>

namespace {

model::Action read(std::string address) {
    return model::Action{model::ActionKind::Read, std::move(address), ""};
}

model::Action write(std::string address) {
    return model::Action{model::ActionKind::Write, std::move(address), ""};
}

model::Action lock(std::string mutex) {
    return model::Action{model::ActionKind::Lock, "", std::move(mutex)};
}

model::Action unlock(std::string mutex) {
    return model::Action{model::ActionKind::Unlock, "", std::move(mutex)};
}

model::Action yield() {
    return model::Action{model::ActionKind::Yield, "", ""};
}

void assert_step(const model::ScheduleStep& step, model::ThreadId thread, std::uint32_t action_index) {
    assert(step.thread == thread);
    assert(step.action_index == action_index);
}

bool throws_invalid_schedule(const model::ModelChecker& checker, const model::Schedule& schedule) {
    try {
        (void)checker.replay(schedule);
    } catch (const std::invalid_argument& error) {
        return std::string(error.what()).find("schedule") != std::string::npos;
    }
    return false;
}

void detects_unprotected_write_write_race_and_replays_it() {
    model::Program program;
    program.threads = {
        {write("x")},
        {write("x")},
    };

    const model::ModelChecker checker(program);
    const auto result = checker.explore_naive();

    assert(result.schedules_explored == 2);
    assert(result.first_race.has_value());
    assert(!result.first_deadlock.has_value());
    assert(result.first_race->address == "x");
    assert(result.first_race->first.thread == 0);
    assert(result.first_race->first.action_index == 0);
    assert(result.first_race->second.thread == 1);
    assert(result.first_race->second.action_index == 0);
    assert(result.first_race->schedule.size() == 2);
    assert_step(result.first_race->schedule[0], 0, 0);
    assert_step(result.first_race->schedule[1], 1, 0);

    const auto replay = checker.replay(result.first_race->schedule);
    assert(replay.first_race.has_value());
    assert(replay.first_race->address == result.first_race->address);
    assert(replay.first_race->first == result.first_race->first);
    assert(replay.first_race->second == result.first_race->second);
    assert(replay.first_race->schedule == result.first_race->schedule);
}

void suppresses_race_when_same_mutex_protects_accesses() {
    model::Program program;
    program.threads = {
        {lock("m"), write("x"), unlock("m")},
        {lock("m"), write("x"), unlock("m")},
    };

    const model::ModelChecker checker(program);
    const auto result = checker.explore_naive();

    assert(result.schedules_explored == 2);
    assert(!result.first_race.has_value());
    assert(!result.first_deadlock.has_value());
    assert(!result.first_error.has_value());
}

void detects_ab_ba_deadlock_and_replays_it() {
    model::Program program;
    program.threads = {
        {lock("a"), lock("b"), unlock("b"), unlock("a")},
        {lock("b"), lock("a"), unlock("a"), unlock("b")},
    };

    const model::ModelChecker checker(program);
    const auto result = checker.explore_naive();

    assert(result.first_deadlock.has_value());
    assert(!result.first_error.has_value());
    assert(result.first_deadlock->schedule.size() == 2);
    assert_step(result.first_deadlock->schedule[0], 0, 0);
    assert_step(result.first_deadlock->schedule[1], 1, 0);
    assert(result.first_deadlock->blocked_threads.size() == 2);
    assert(result.first_deadlock->blocked_threads[0].thread == 0);
    assert(result.first_deadlock->blocked_threads[0].mutex == "b");
    assert(result.first_deadlock->blocked_threads[0].owner.has_value());
    assert(*result.first_deadlock->blocked_threads[0].owner == 1);
    assert(result.first_deadlock->blocked_threads[1].thread == 1);
    assert(result.first_deadlock->blocked_threads[1].mutex == "a");
    assert(result.first_deadlock->blocked_threads[1].owner.has_value());
    assert(*result.first_deadlock->blocked_threads[1].owner == 0);

    const auto deadlock_replay = checker.replay(result.first_deadlock->schedule);
    assert(deadlock_replay.schedules_explored == 1);
    assert(deadlock_replay.first_deadlock.has_value());
    assert(deadlock_replay.first_deadlock->blocked_threads == result.first_deadlock->blocked_threads);
    assert(deadlock_replay.first_deadlock->schedule == result.first_deadlock->schedule);

    const model::Schedule clean_schedule = {
        {0, 0}, {0, 1}, {0, 2}, {0, 3},
        {1, 0}, {1, 1}, {1, 2}, {1, 3},
    };
    const auto clean_replay = checker.replay(clean_schedule);
    assert(clean_replay.schedules_explored == 1);
    assert(!clean_replay.first_race.has_value());
    assert(!clean_replay.first_deadlock.has_value());
    assert(!clean_replay.first_error.has_value());
}

void ordered_locking_never_deadlocks() {
    model::Program program;
    program.threads = {
        {lock("a"), lock("b"), unlock("b"), unlock("a")},
        {lock("a"), lock("b"), unlock("b"), unlock("a")},
    };

    const model::ModelChecker checker(program);
    const auto result = checker.explore_naive();

    assert(result.schedules_explored == 2);
    assert(!result.first_deadlock.has_value());
    assert(!result.first_error.has_value());
}

void read_read_same_address_is_not_a_race() {
    model::Program program;
    program.threads = {
        {read("x")},
        {read("x")},
    };

    const model::ModelChecker checker(program);
    const auto result = checker.explore_naive();

    assert(result.schedules_explored == 2);
    assert(!result.first_race.has_value());
}

void counts_all_always_enabled_interleavings() {
    model::Program program;
    program.threads = {
        {yield(), yield()},
        {yield(), yield()},
    };

    const model::ModelChecker checker(program);
    const auto result = checker.explore_naive();

    assert(result.schedules_explored == 6);
    assert(!result.first_race.has_value());
    assert(!result.first_deadlock.has_value());
    assert(!result.first_error.has_value());
}

void rejects_invalid_replay_schedules() {
    model::Program program;
    program.threads = {
        {lock("m"), unlock("m")},
        {lock("m"), unlock("m")},
    };

    const model::ModelChecker checker(program);
    assert(throws_invalid_schedule(checker, {{2, 0}}));
    assert(throws_invalid_schedule(checker, {{0, 1}}));
    assert(throws_invalid_schedule(checker, {{0, 0}, {1, 0}}));
}

void reports_non_owner_unlock_as_modeled_error() {
    model::Program program;
    program.threads = {
        {unlock("m")},
    };

    const model::ModelChecker checker(program);
    const auto result = checker.explore_naive();

    assert(result.schedules_explored == 1);
    assert(result.first_error.has_value());
    assert(result.first_error->endpoint.thread == 0);
    assert(result.first_error->endpoint.action_index == 0);
    assert(result.first_error->schedule.size() == 1);

    const auto replay = checker.replay(result.first_error->schedule);
    assert(replay.schedules_explored == 1);
    assert(replay.first_error.has_value());
    assert(replay.first_error->endpoint == result.first_error->endpoint);
}

void detects_race_when_only_one_side_locks() {
    // Protects INVARIANTS.md soundness: locking on one side only does not
    // order the accesses, so the race must still be reported.
    model::Program program;
    program.threads = {
        {lock("m"), write("x"), unlock("m")},
        {write("x")},
    };

    const model::ModelChecker checker(program);
    const auto result = checker.explore_naive();

    assert(result.first_race.has_value());
    assert(result.first_race->address == "x");

    const auto replay = checker.replay(result.first_race->schedule);
    assert(replay.first_race.has_value());
    assert(*replay.first_race == *result.first_race);
}

void detects_race_when_accesses_use_different_mutexes() {
    // Protects INVARIANTS.md soundness: distinct mutexes create no
    // happens-before edge between the critical sections, so the conflicting
    // writes remain unordered and must be reported.
    model::Program program;
    program.threads = {
        {lock("m"), write("x"), unlock("m")},
        {lock("n"), write("x"), unlock("n")},
    };

    const model::ModelChecker checker(program);
    const auto result = checker.explore_naive();

    assert(result.first_race.has_value());
    assert(result.first_race->address == "x");
}

void detects_read_write_race() {
    model::Program program;
    program.threads = {
        {read("x")},
        {write("x")},
    };

    const model::ModelChecker checker(program);
    const auto result = checker.explore_naive();

    assert(result.first_race.has_value());
    assert(result.first_race->address == "x");
}

} // namespace

int main() {
    detects_unprotected_write_write_race_and_replays_it();
    suppresses_race_when_same_mutex_protects_accesses();
    detects_ab_ba_deadlock_and_replays_it();
    ordered_locking_never_deadlocks();
    read_read_same_address_is_not_a_race();
    counts_all_always_enabled_interleavings();
    rejects_invalid_replay_schedules();
    reports_non_owner_unlock_as_modeled_error();
    detects_race_when_only_one_side_locks();
    detects_race_when_accesses_use_different_mutexes();
    detects_read_write_race();
    return 0;
}
