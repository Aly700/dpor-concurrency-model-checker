#include "model/checker.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
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

model::Action atomic_load(std::string address) {
    model::Action action;
    action.kind = model::ActionKind::AtomicLoad;
    action.address = std::move(address);
    return action;
}

model::Action atomic_store(std::string address) {
    model::Action action;
    action.kind = model::ActionKind::AtomicStore;
    action.address = std::move(address);
    return action;
}

model::Action lock(std::string mutex) {
    model::Action action;
    action.kind = model::ActionKind::Lock;
    action.mutex = std::move(mutex);
    return action;
}

model::Action try_lock(std::string mutex, model::RegisterId destination = 0) {
    model::Action action;
    action.kind = model::ActionKind::TryLock;
    action.mutex = std::move(mutex);
    action.destination = destination;
    return action;
}

model::Action unlock(std::string mutex) {
    model::Action action;
    action.kind = model::ActionKind::Unlock;
    action.mutex = std::move(mutex);
    return action;
}

model::Action fence() {
    model::Action action;
    action.kind = model::ActionKind::Fence;
    return action;
}

model::Action barrier_wait(std::string barrier, std::uint32_t parties) {
    model::Action action;
    action.kind = model::ActionKind::BarrierWait;
    action.barrier = std::move(barrier);
    action.parties = parties;
    return action;
}

model::ValueOperand imm(model::Value value) {
    model::ValueOperand operand;
    operand.kind = model::ValueOperandKind::Immediate;
    operand.immediate = value;
    return operand;
}

model::Action set(model::RegisterId reg, model::Value value) {
    model::Action action;
    action.kind = model::ActionKind::Set;
    action.destination = reg;
    action.value = imm(value);
    return action;
}

model::Action label(std::string name) {
    model::Action action;
    action.kind = model::ActionKind::Label;
    action.label = std::move(name);
    return action;
}

model::Action bnz(model::RegisterId reg, std::string target) {
    model::Action action;
    action.kind = model::ActionKind::BranchNonzero;
    action.source_register = reg;
    action.label = std::move(target);
    return action;
}

const std::array<model::Action, 13> kActions{
    read("x"),
    read("y"),
    write("x"),
    write("y"),
    atomic_load("x"),
    atomic_load("y"),
    atomic_store("x"),
    atomic_store("y"),
    lock("m"),
    try_lock("m"),
    unlock("m"),
    fence(),
    barrier_wait("bar", 2),
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

std::uint64_t pow_actions(std::size_t exponent) {
    std::uint64_t result = 1;
    for (std::size_t i = 0; i < exponent; ++i) {
        result *= kActions.size();
    }
    return result;
}

bool bound_hit(const model::CheckResult& result) {
    return result.bound_exceeded_executions > 0;
}

bool cycle_exists(const model::CheckResult& result) {
    return result.cycles_detected > 0;
}

bool fair_cycle_exists(const model::CheckResult& result) {
    return result.fair_cycles > 0;
}

bool unfair_cycle_exists(const model::CheckResult& result) {
    return result.unfair_cycles > 0;
}

struct CycleCounts {
    std::size_t naive_fair{0};
    std::size_t dpor_fair{0};
    std::size_t naive_unfair{0};
    std::size_t dpor_unfair{0};
};

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
    if (dpor.first_assertion.has_value()) {
        const auto replay = checker.replay(dpor.first_assertion->schedule);
        assert(replay.first_assertion.has_value());
        assert(*replay.first_assertion == *dpor.first_assertion);
    }
    if (dpor.first_nontermination.has_value()) {
        const auto replay = checker.replay(dpor.first_nontermination->schedule);
        assert(replay.first_nontermination.has_value());
        assert(*replay.first_nontermination == *dpor.first_nontermination);
    }
}

std::string action_string(const model::Action& action) {
    std::ostringstream out;
    out << static_cast<int>(action.kind);
    if (!action.address.empty()) {
        out << " " << action.address;
    }
    if (!action.mutex.empty()) {
        out << " " << action.mutex;
    }
    return out.str();
}

void print_program(const model::Program& program) {
    for (std::size_t tid = 0; tid < program.threads.size(); ++tid) {
        std::cerr << "  t" << tid << ':';
        for (const auto& action : program.threads[tid]) {
            std::cerr << " [" << action_string(action) << ']';
        }
        std::cerr << '\n';
    }
}

void cross_validate_program(const model::Program& program,
                            std::size_t& programs_checked,
                            std::size_t& skipped_capped,
                            std::size_t& naive_total,
                            std::size_t& dpor_total,
                            CycleCounts& cycle_counts) {
    constexpr std::size_t kStepBound = 20;
    constexpr std::size_t kMaxSchedules = 50000;
    const model::ModelChecker checker(program, kStepBound, model::MemoryModel::TSO);
    const auto naive = checker.explore_naive(kMaxSchedules);
    const auto dpor = checker.explore_dpor(kMaxSchedules);

    if (naive.exploration_capped || dpor.exploration_capped) {
        ++skipped_capped;
        return;
    }

    if (dpor.first_race.has_value() != naive.first_race.has_value() ||
        dpor.first_deadlock.has_value() != naive.first_deadlock.has_value() ||
        dpor.first_error.has_value() != naive.first_error.has_value() ||
        dpor.first_assertion.has_value() != naive.first_assertion.has_value() ||
        cycle_exists(dpor) != cycle_exists(naive) ||
        fair_cycle_exists(dpor) != fair_cycle_exists(naive) ||
        unfair_cycle_exists(dpor) != unfair_cycle_exists(naive) ||
        bound_hit(dpor) != bound_hit(naive) ||
        dpor.schedules_explored > naive.schedules_explored) {
        std::cerr << "TSO oracle mismatch\n";
        print_program(program);
        std::cerr << "  naive schedules=" << naive.schedules_explored
                  << " race=" << naive.first_race.has_value()
                  << " deadlock=" << naive.first_deadlock.has_value()
                  << " error=" << naive.first_error.has_value()
                  << " assertion=" << naive.first_assertion.has_value()
                  << " cycle=" << cycle_exists(naive)
                  << " fair_cycle=" << fair_cycle_exists(naive)
                  << " unfair_cycle=" << unfair_cycle_exists(naive)
                  << " bound=" << bound_hit(naive) << '\n';
        std::cerr << "  dpor schedules=" << dpor.schedules_explored
                  << " race=" << dpor.first_race.has_value()
                  << " deadlock=" << dpor.first_deadlock.has_value()
                  << " error=" << dpor.first_error.has_value()
                  << " assertion=" << dpor.first_assertion.has_value()
                  << " cycle=" << cycle_exists(dpor)
                  << " fair_cycle=" << fair_cycle_exists(dpor)
                  << " unfair_cycle=" << unfair_cycle_exists(dpor)
                  << " bound=" << bound_hit(dpor) << '\n';
        std::abort();
    }

    assert_replays_dpor_report(checker, dpor);
    ++programs_checked;
    naive_total += naive.schedules_explored;
    dpor_total += dpor.schedules_explored;
    cycle_counts.naive_fair += naive.fair_cycles;
    cycle_counts.dpor_fair += dpor.fair_cycles;
    cycle_counts.naive_unfair += naive.unfair_cycles;
    cycle_counts.dpor_unfair += dpor.unfair_cycles;
}

} // namespace

int main() {
    if (kActions.size() != 13) {
        throw std::runtime_error("TSO oracle alphabet must include TryLock");
    }
    const model::Action& try_lock_entry = kActions.at(9);
    if (try_lock_entry.kind != model::ActionKind::TryLock ||
        try_lock_entry.mutex != "m" ||
        !try_lock_entry.destination.has_value() ||
        *try_lock_entry.destination != 0) {
        throw std::runtime_error(
            "TSO oracle TryLock entry must be try_lock m -> r0");
    }
    std::size_t programs_checked = 0;
    std::size_t skipped_capped = 0;
    std::size_t naive_total = 0;
    std::size_t dpor_total = 0;
    CycleCounts cycle_counts;

    constexpr std::uint64_t kProgramsPerLengthPairCap = 1024;
    for (std::size_t lhs_length = 0; lhs_length <= 3; ++lhs_length) {
        for (std::size_t rhs_length = 0; rhs_length <= 3; ++rhs_length) {
            const auto count = pow_actions(lhs_length + rhs_length);
            const auto samples = std::min<std::uint64_t>(count, kProgramsPerLengthPairCap);
            for (std::uint64_t sample = 0; sample < samples; ++sample) {
                const auto encoded = count == samples ? sample : (sample * count) / samples;
                cross_validate_program(
                    two_thread_program(encoded, lhs_length, rhs_length),
                    programs_checked,
                    skipped_capped,
                    naive_total,
                    dpor_total,
                    cycle_counts);
            }
        }
    }

    // Before thread 1 publishes and flushes x, it is continuously enabled and
    // the spin witness is unfair. After its buffer drains and it finishes, the
    // spinner supplies a fair-divergence witness. This exercises both class
    // existence gates as well as exact replay.
    cross_validate_program(model::Program{{
                               {set(1, 1), label("spin"), bnz(1, "spin")},
                               {atomic_store("x")},
                           }},
                           programs_checked,
                           skipped_capped,
                           naive_total,
                           dpor_total,
                           cycle_counts);

    std::cout << "tso_oracle: programs checked=" << programs_checked
              << " skipped_capped=" << skipped_capped
              << " alphabet=" << kActions.size()
              << " cap_per_length_pair=" << kProgramsPerLengthPairCap
              << " naive schedules total=" << naive_total
              << " dpor schedules total=" << dpor_total
              << " naive_fair_cycles=" << cycle_counts.naive_fair
              << " dpor_fair_cycles=" << cycle_counts.dpor_fair
              << " naive_unfair_cycles=" << cycle_counts.naive_unfair
              << " dpor_unfair_cycles=" << cycle_counts.dpor_unfair << '\n';
    if (skipped_capped != 0) {
        throw std::runtime_error("TSO oracle must not skip capped programs");
    }
    return 0;
}
