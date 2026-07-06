#include "model/checker.hpp"

#include <array>
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
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

std::string action_string(const model::Action& action) {
    std::ostringstream out;
    switch (action.kind) {
    case model::ActionKind::Read:
        out << "Read " << action.address;
        break;
    case model::ActionKind::Write:
        out << "Write " << action.address;
        break;
    case model::ActionKind::Lock:
        out << "Lock " << action.mutex;
        break;
    case model::ActionKind::Unlock:
        out << "Unlock " << action.mutex;
        break;
    case model::ActionKind::Yield:
        out << "Yield";
        break;
    }
    return out.str();
}

void print_program(const model::Program& program) {
    for (std::size_t tid = 0; tid < program.threads.size(); ++tid) {
        std::cerr << "  t" << tid << ':';
        for (const auto& action : program.threads[tid]) {
            std::cerr << ' ' << '[' << action_string(action) << ']';
        }
        std::cerr << '\n';
    }
}

std::uint64_t pow8(std::size_t exponent) {
    std::uint64_t result = 1;
    for (std::size_t i = 0; i < exponent; ++i) {
        result *= kActions.size();
    }
    return result;
}

void assert_replays_dpor_report(const model::ModelChecker& checker, const model::CheckResult& dpor) {
    if (dpor.first_race.has_value()) {
        const auto replay = checker.replay(dpor.first_race->schedule);
        assert(replay.first_race.has_value());
        assert(*replay.first_race == *dpor.first_race);
    }

    if (dpor.first_deadlock.has_value()) {
        const auto replay = checker.replay(dpor.first_deadlock->schedule);
        assert(replay.first_deadlock.has_value());
        assert(*replay.first_deadlock == *dpor.first_deadlock);
    }

    if (dpor.first_error.has_value()) {
        const auto replay = checker.replay(dpor.first_error->schedule);
        assert(replay.first_error.has_value());
        assert(*replay.first_error == *dpor.first_error);
    }
}

void cross_validate_program(const model::Program& program,
                            std::size_t& programs_checked,
                            std::size_t& naive_total,
                            std::size_t& dpor_total) {
    const model::ModelChecker checker(program);
    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();

    if (dpor.first_race.has_value() != naive.first_race.has_value() ||
        dpor.first_deadlock.has_value() != naive.first_deadlock.has_value() ||
        dpor.first_error.has_value() != naive.first_error.has_value() ||
        dpor.schedules_explored > naive.schedules_explored) {
        std::cerr << "oracle mismatch\n";
        print_program(program);
        std::cerr << "  naive schedules=" << naive.schedules_explored
                  << " race=" << naive.first_race.has_value()
                  << " deadlock=" << naive.first_deadlock.has_value()
                  << " error=" << naive.first_error.has_value() << '\n';
        std::cerr << "  dpor schedules=" << dpor.schedules_explored
                  << " race=" << dpor.first_race.has_value()
                  << " deadlock=" << dpor.first_deadlock.has_value()
                  << " error=" << dpor.first_error.has_value() << '\n';
    }

    assert(dpor.first_race.has_value() == naive.first_race.has_value());
    assert(dpor.first_deadlock.has_value() == naive.first_deadlock.has_value());
    assert(dpor.first_error.has_value() == naive.first_error.has_value());
    assert(dpor.schedules_explored <= naive.schedules_explored);
    assert_replays_dpor_report(checker, dpor);

    ++programs_checked;
    naive_total += naive.schedules_explored;
    dpor_total += dpor.schedules_explored;
}

void assert_strict_reduction(const model::Program& program) {
    const model::ModelChecker checker(program);
    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    assert(dpor.schedules_explored < naive.schedules_explored);
}

std::vector<model::Program> hand_picked_programs() {
    return {
        model::Program{{
            {lock("a"), lock("b"), unlock("b"), unlock("a")},
            {lock("b"), lock("c"), unlock("c"), unlock("b")},
            {lock("c"), lock("a"), unlock("a"), unlock("c")},
        }},
        model::Program{{
            {write("x"), read("y")},
            {read("x"), write("y")},
            {read("y"), write("x")},
        }},
        model::Program{{
            {lock("m"), unlock("m")},
            {lock("m"), lock("n"), unlock("n"), unlock("m")},
            {lock("n"), lock("m"), unlock("m"), unlock("n")},
        }},
    };
}

void assert_disabled_transition_fallback_finds_deadlock() {
    // At the state before thread 0 unlocks m, thread 1 is disabled on m.
    // The dependent later lock by thread 1 must therefore add every enabled
    // thread at that point, including thread 2, or the m/n deadlock is missed.
    const model::Program program{{
        {lock("m"), unlock("m")},
        {lock("m"), lock("n"), unlock("n"), unlock("m")},
        {lock("n"), lock("m"), unlock("m"), unlock("n")},
    }};
    const model::ModelChecker checker(program);
    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    assert(naive.first_deadlock.has_value());
    assert(dpor.first_deadlock.has_value());
    assert_replays_dpor_report(checker, dpor);
}

} // namespace

int main() {
    std::size_t programs_checked = 0;
    std::size_t naive_total = 0;
    std::size_t dpor_total = 0;

    // Deterministically enumerates two-thread programs with 0..3 actions per
    // thread over kActions. Each length pair is capped at 4096 programs; larger
    // length pairs use evenly spaced encoded indexes, not randomness.
    constexpr std::uint64_t kProgramsPerLengthPairCap = 4096;
    for (std::size_t lhs_length = 0; lhs_length <= 3; ++lhs_length) {
        for (std::size_t rhs_length = 0; rhs_length <= 3; ++rhs_length) {
            const auto count = pow8(lhs_length + rhs_length);
            const auto samples = std::min<std::uint64_t>(count, kProgramsPerLengthPairCap);
            for (std::uint64_t sample = 0; sample < samples; ++sample) {
                const auto encoded = count == samples ? sample : (sample * count) / samples;
                cross_validate_program(
                    two_thread_program(encoded, lhs_length, rhs_length),
                    programs_checked,
                    naive_total,
                    dpor_total);
            }
        }
    }

    for (const auto& program : hand_picked_programs()) {
        cross_validate_program(program, programs_checked, naive_total, dpor_total);
    }
    assert_disabled_transition_fallback_finds_deadlock();

    assert_strict_reduction(model::Program{{
        {yield(), yield(), yield()},
        {yield(), yield(), yield()},
    }});
    assert_strict_reduction(model::Program{{
        {write("x"), write("y")},
        {yield(), yield()},
    }});
    assert_strict_reduction(model::Program{{
        {lock("m"), write("x"), unlock("m")},
        {yield(), yield(), yield()},
    }});

    std::cout << "dpor_oracle: programs checked=" << programs_checked
              << " naive schedules total=" << naive_total
              << " dpor schedules total=" << dpor_total << '\n';
    return 0;
}
