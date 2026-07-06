#include "model/checker.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

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

bool same_race_identity(const model::RaceReport& lhs, const model::RaceReport& rhs) {
    if (lhs.address != rhs.address) {
        return false;
    }

    return (lhs.first == rhs.first && lhs.second == rhs.second) ||
           (lhs.first == rhs.second && lhs.second == rhs.first);
}

std::vector<model::BlockedThread> canonical_blocked(std::vector<model::BlockedThread> blocked) {
    std::sort(blocked.begin(), blocked.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.thread != rhs.thread) {
            return lhs.thread < rhs.thread;
        }
        if (lhs.mutex != rhs.mutex) {
            return lhs.mutex < rhs.mutex;
        }
        if (lhs.owner.has_value() != rhs.owner.has_value()) {
            return !lhs.owner.has_value();
        }
        return lhs.owner.value_or(0) < rhs.owner.value_or(0);
    });
    return blocked;
}

bool same_deadlock_identity(const model::DeadlockReport& lhs, const model::DeadlockReport& rhs) {
    return canonical_blocked(lhs.blocked_threads) == canonical_blocked(rhs.blocked_threads);
}

bool same_error_identity(const model::ModelErrorReport& lhs, const model::ModelErrorReport& rhs) {
    return lhs.endpoint == rhs.endpoint;
}

void assert_fixed_point(const model::ModelChecker& checker, const model::Schedule& schedule) {
    assert(checker.minimize_schedule(schedule) == schedule);
}

void minimizes_padded_race_schedule_and_explore_reports_it_minimized() {
    const model::Program program{{
        {write("x"), yield(), write("y")},
        {yield(), write("z")},
        {write("x")},
    }};
    const model::Schedule padded = {
        {0, 0}, {0, 1}, {0, 2}, {1, 0}, {1, 1}, {2, 0},
    };

    const model::ModelChecker checker(program);
    const auto raw = checker.replay(padded);
    assert(raw.first_race.has_value());

    const auto minimized = checker.minimize_schedule(padded);
    assert(minimized.size() < padded.size());
    const auto replay = checker.replay(minimized);
    assert(replay.first_race.has_value());
    assert(same_race_identity(*raw.first_race, *replay.first_race));
    assert_fixed_point(checker, minimized);

    const auto explored = checker.explore_naive();
    assert(explored.first_race.has_value());
    assert(same_race_identity(*raw.first_race, *explored.first_race));
    assert(explored.first_race->schedule == minimized);
}

void preserves_padded_deadlock_identity_as_a_fixed_point() {
    const model::Program program{{
        {lock("a"), lock("b")},
        {yield()},
        {lock("b"), lock("a")},
    }};
    const model::Schedule padded = {
        {0, 0}, {1, 0}, {2, 0},
    };

    const model::ModelChecker checker(program);
    const auto raw = checker.replay(padded);
    assert(raw.first_deadlock.has_value());

    const auto minimized = checker.minimize_schedule(padded);
    assert(minimized.size() <= padded.size());
    const auto replay = checker.replay(minimized);
    assert(replay.first_deadlock.has_value());
    assert(same_deadlock_identity(*raw.first_deadlock, *replay.first_deadlock));
    assert_fixed_point(checker, minimized);
}

void minimizes_padded_modeled_error_schedule() {
    const model::Program program{{
        {unlock("m")},
        {yield(), write("z")},
    }};
    const model::Schedule padded = {
        {1, 0}, {1, 1}, {0, 0},
    };

    const model::ModelChecker checker(program);
    const auto raw = checker.replay(padded);
    assert(raw.first_error.has_value());

    const auto minimized = checker.minimize_schedule(padded);
    assert(minimized.size() < padded.size());
    const auto replay = checker.replay(minimized);
    assert(replay.first_error.has_value());
    assert(same_error_identity(*raw.first_error, *replay.first_error));
    assert_fixed_point(checker, minimized);
}

void leaves_non_buggy_schedules_unchanged() {
    const model::Program program{{
        {yield()},
        {yield()},
    }};
    const model::Schedule schedule = {
        {0, 0}, {1, 0},
    };

    const model::ModelChecker checker(program);
    const auto replay = checker.replay(schedule);
    assert(!replay.first_race.has_value());
    assert(!replay.first_deadlock.has_value());
    assert(!replay.first_error.has_value());
    assert(checker.minimize_schedule(schedule) == schedule);
}

const std::array<model::Action, 8> kActions{
    read("x"),
    write("x"),
    write("y"),
    lock("m"),
    lock("n"),
    unlock("m"),
    unlock("n"),
    yield(),
};

model::Program two_thread_program(std::uint64_t encoded, std::size_t lhs_length, std::size_t rhs_length) {
    model::Program program;
    program.threads.resize(2);
    for (std::size_t i = 0; i < lhs_length; ++i) {
        program.threads[0].push_back(kActions.at(encoded % kActions.size()));
        encoded /= kActions.size();
    }
    for (std::size_t i = 0; i < rhs_length; ++i) {
        program.threads[1].push_back(kActions.at(encoded % kActions.size()));
        encoded /= kActions.size();
    }
    return program;
}

std::uint64_t pow8(std::size_t exponent) {
    std::uint64_t result = 1;
    for (std::size_t i = 0; i < exponent; ++i) {
        result *= kActions.size();
    }
    return result;
}

void assert_minimized_dpor_report_schedules_preserve_identity() {
    constexpr std::uint64_t kProgramsPerLengthPairCap = 512;

    for (std::size_t lhs_length = 0; lhs_length <= 3; ++lhs_length) {
        for (std::size_t rhs_length = 0; rhs_length <= 3; ++rhs_length) {
            const auto count = pow8(lhs_length + rhs_length);
            const auto samples = std::min<std::uint64_t>(count, kProgramsPerLengthPairCap);
            for (std::uint64_t sample = 0; sample < samples; ++sample) {
                const auto encoded = count == samples ? sample : (sample * count) / samples;
                const model::ModelChecker checker(two_thread_program(encoded, lhs_length, rhs_length));
                const auto dpor = checker.explore_dpor();

                if (dpor.first_race.has_value()) {
                    const auto minimized = checker.minimize_schedule(dpor.first_race->schedule);
                    assert(minimized.size() <= dpor.first_race->schedule.size());
                    const auto replay = checker.replay(minimized);
                    assert(replay.first_race.has_value());
                    assert(same_race_identity(*dpor.first_race, *replay.first_race));
                    assert_fixed_point(checker, minimized);
                }

                if (dpor.first_deadlock.has_value()) {
                    const auto minimized = checker.minimize_schedule(dpor.first_deadlock->schedule);
                    assert(minimized.size() <= dpor.first_deadlock->schedule.size());
                    const auto replay = checker.replay(minimized);
                    assert(replay.first_deadlock.has_value());
                    assert(same_deadlock_identity(*dpor.first_deadlock, *replay.first_deadlock));
                    assert_fixed_point(checker, minimized);
                }

                if (dpor.first_error.has_value()) {
                    const auto minimized = checker.minimize_schedule(dpor.first_error->schedule);
                    assert(minimized.size() <= dpor.first_error->schedule.size());
                    const auto replay = checker.replay(minimized);
                    assert(replay.first_error.has_value());
                    assert(same_error_identity(*dpor.first_error, *replay.first_error));
                    assert_fixed_point(checker, minimized);
                }
            }
        }
    }
}

} // namespace

int main() {
    minimizes_padded_race_schedule_and_explore_reports_it_minimized();
    preserves_padded_deadlock_identity_as_a_fixed_point();
    minimizes_padded_modeled_error_schedule();
    leaves_non_buggy_schedules_unchanged();
    assert_minimized_dpor_report_schedules_preserve_identity();
    return 0;
}
