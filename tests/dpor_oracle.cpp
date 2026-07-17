#include "model/checker.hpp"

#ifdef DPOR_SYMMETRY_DIAGNOSIS
#include "symmetry_diagnosis.hpp"

#include <chrono>
#endif

#include <array>
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
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

model::Action atomic_rmw(std::string address) {
    model::Action action;
    action.kind = model::ActionKind::AtomicRmw;
    action.address = std::move(address);
    return action;
}

model::Action lock(std::string mutex) {
    return model::Action{model::ActionKind::Lock, "", std::move(mutex)};
}

model::Action try_lock(std::string mutex, model::RegisterId destination = 0) {
    model::Action action;
    action.kind = model::ActionKind::TryLock;
    action.mutex = std::move(mutex);
    action.destination = destination;
    return action;
}

model::Action unlock(std::string mutex) {
    return model::Action{model::ActionKind::Unlock, "", std::move(mutex)};
}

model::Action rwlock_action(model::ActionKind kind, std::string rwlock) {
    model::Action action;
    action.kind = kind;
    action.rwlock = std::move(rwlock);
    return action;
}

model::Action rlock(std::string rwlock) {
    return rwlock_action(model::ActionKind::RLock, std::move(rwlock));
}

model::Action runlock(std::string rwlock) {
    return rwlock_action(model::ActionKind::RUnlock, std::move(rwlock));
}

model::Action wlock(std::string rwlock) {
    return rwlock_action(model::ActionKind::WLock, std::move(rwlock));
}

model::Action wunlock(std::string rwlock) {
    return rwlock_action(model::ActionKind::WUnlock, std::move(rwlock));
}

model::Action semaphore_action(model::ActionKind kind, std::string semaphore) {
    model::Action action;
    action.kind = kind;
    action.semaphore = std::move(semaphore);
    return action;
}

model::Action sem_post(std::string semaphore) {
    return semaphore_action(model::ActionKind::SemPost, std::move(semaphore));
}

model::Action sem_wait(std::string semaphore) {
    return semaphore_action(model::ActionKind::SemWait, std::move(semaphore));
}

model::Action barrier_wait(std::string barrier, std::uint32_t parties) {
    model::Action action;
    action.kind = model::ActionKind::BarrierWait;
    action.barrier = std::move(barrier);
    action.parties = parties;
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

model::Action yield() {
    return model::Action{model::ActionKind::Yield, "", ""};
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

const std::array<model::Action, 25> kActions{
    read("x"),
    write("x"),
    write("y"),
    atomic_load("f"),
    atomic_store("f"),
    atomic_rmw("f"),
    lock("m"),
    lock("n"),
    try_lock("m"),
    unlock("m"),
    unlock("n"),
    wait("cv", "m"),
    signal("cv"),
    broadcast("cv"),
    spawn(1),
    join(0),
    join(1),
    yield(),
    rlock("rw"),
    runlock("rw"),
    wlock("rw"),
    wunlock("rw"),
    sem_post("sem"),
    sem_wait("sem"),
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

std::string action_string(const model::Action& action) {
    std::ostringstream out;
    switch (action.kind) {
    case model::ActionKind::Set:
        out << "Set";
        break;
    case model::ActionKind::Label:
        out << "Label " << action.label;
        break;
    case model::ActionKind::BranchNonzero:
        out << "BranchNonzero";
        break;
    case model::ActionKind::Assert:
        out << "Assert";
        break;
    case model::ActionKind::Read:
        out << "Read " << action.address;
        break;
    case model::ActionKind::Write:
        out << "Write " << action.address;
        break;
    case model::ActionKind::AtomicLoad:
        out << "AtomicLoad " << action.address;
        break;
    case model::ActionKind::AtomicStore:
        out << "AtomicStore " << action.address;
        break;
    case model::ActionKind::AtomicRmw:
        out << "AtomicRmw " << action.address;
        break;
    case model::ActionKind::CompareExchange:
        out << "CompareExchange " << action.address;
        break;
    case model::ActionKind::Fence:
        out << "Fence";
        break;
    case model::ActionKind::Flush:
        out << "Flush " << action.address;
        break;
    case model::ActionKind::Lock:
        out << "Lock " << action.mutex;
        break;
    case model::ActionKind::TryLock:
        out << "TryLock " << action.mutex << " -> r"
            << static_cast<unsigned>(action.destination.value_or(0));
        break;
    case model::ActionKind::Unlock:
        out << "Unlock " << action.mutex;
        break;
    case model::ActionKind::Spawn:
        out << "Spawn " << action.target;
        break;
    case model::ActionKind::Join:
        out << "Join " << action.target;
        break;
    case model::ActionKind::Wait:
        out << "Wait " << action.condition << ' ' << action.mutex;
        break;
    case model::ActionKind::Signal:
        out << "Signal " << action.condition;
        break;
    case model::ActionKind::Broadcast:
        out << "Broadcast " << action.condition;
        break;
    case model::ActionKind::Yield:
        out << "Yield";
        break;
    case model::ActionKind::RLock:
        out << "RLock " << action.rwlock;
        break;
    case model::ActionKind::RUnlock:
        out << "RUnlock " << action.rwlock;
        break;
    case model::ActionKind::WLock:
        out << "WLock " << action.rwlock;
        break;
    case model::ActionKind::WUnlock:
        out << "WUnlock " << action.rwlock;
        break;
    case model::ActionKind::SemPost:
        out << "SemPost " << action.semaphore;
        break;
    case model::ActionKind::SemWait:
        out << "SemWait " << action.semaphore;
        break;
    case model::ActionKind::BarrierWait:
        out << "BarrierWait " << action.barrier << ' ' << action.parties;
        break;
    }
    return out.str();
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

bool strongly_unfair_cycle_exists(const model::CheckResult& result) {
    return result.strongly_unfair_cycles > 0;
}

bool unfair_cycle_exists(const model::CheckResult& result) {
    return result.unfair_cycles > 0;
}

struct CycleCounts {
    std::size_t naive_fair{0};
    std::size_t dpor_fair{0};
    std::size_t naive_strongly_unfair{0};
    std::size_t dpor_strongly_unfair{0};
    std::size_t naive_unfair{0};
    std::size_t dpor_unfair{0};
};

void print_program(const model::Program& program) {
    for (std::size_t tid = 0; tid < program.threads.size(); ++tid) {
        std::cerr << "  t" << tid << ':';
        for (const auto& action : program.threads[tid]) {
            std::cerr << ' ' << '[' << action_string(action) << ']';
        }
        std::cerr << '\n';
    }
}

std::uint64_t pow_actions(std::size_t exponent) {
    std::uint64_t result = 1;
    for (std::size_t i = 0; i < exponent; ++i) {
        result *= kActions.size();
    }
    return result;
}

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void assert_replays_dpor_report(const model::ModelChecker& checker, const model::CheckResult& dpor) {
    if (dpor.first_race.has_value()) {
        const auto replay = checker.replay(dpor.first_race->schedule);
        require(replay.first_race.has_value() &&
                    *replay.first_race == *dpor.first_race,
                "two-thread race report did not replay identically");
    }

    if (dpor.first_deadlock.has_value()) {
        const auto replay = checker.replay(dpor.first_deadlock->schedule);
        require(replay.first_deadlock.has_value() &&
                    *replay.first_deadlock == *dpor.first_deadlock,
                "two-thread deadlock report did not replay identically");
    }

    if (dpor.first_error.has_value()) {
        const auto replay = checker.replay(dpor.first_error->schedule);
        require(replay.first_error.has_value() &&
                    *replay.first_error == *dpor.first_error,
                "two-thread error report did not replay identically");
    }

    if (dpor.first_assertion.has_value()) {
        const auto replay = checker.replay(dpor.first_assertion->schedule);
        require(replay.first_assertion.has_value() &&
                    *replay.first_assertion == *dpor.first_assertion,
                "two-thread assertion report did not replay identically");
    }
    if (dpor.first_nontermination.has_value()) {
        const auto replay = checker.replay(dpor.first_nontermination->schedule);
        require(replay.first_nontermination.has_value() &&
                    *replay.first_nontermination == *dpor.first_nontermination,
                "two-thread nontermination report did not replay identically");
    }
}

void cross_validate_program(const model::Program& program,
                            std::size_t& programs_checked,
                            std::size_t& naive_total,
                            std::size_t& dpor_total,
                            CycleCounts& cycle_counts) {
    const model::ModelChecker checker(program);
#ifdef DPOR_SYMMETRY_DIAGNOSIS
    const auto naive_started = std::chrono::steady_clock::now();
#endif
    const auto naive = checker.explore_naive();
#ifdef DPOR_SYMMETRY_DIAGNOSIS
    const auto naive_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - naive_started)
            .count());
    const auto dpor_started = std::chrono::steady_clock::now();
#endif
    const auto dpor = checker.explore_dpor();
#ifdef DPOR_SYMMETRY_DIAGNOSIS
    const auto dpor_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - dpor_started)
            .count());
    symmetry_diagnosis::record_program(
        "dpor_oracle",
        {"memory=sc", "paired"},
        program,
        checker,
        model::MemoryModel::SC,
        naive,
        dpor,
        20000,
        {naive_ns, dpor_ns});
#endif

    if (dpor.first_race.has_value() != naive.first_race.has_value() ||
        dpor.first_deadlock.has_value() != naive.first_deadlock.has_value() ||
        dpor.first_error.has_value() != naive.first_error.has_value() ||
        dpor.first_assertion.has_value() != naive.first_assertion.has_value() ||
        cycle_exists(dpor) != cycle_exists(naive) ||
        fair_cycle_exists(dpor) != fair_cycle_exists(naive) ||
        strongly_unfair_cycle_exists(dpor) !=
            strongly_unfair_cycle_exists(naive) ||
        unfair_cycle_exists(dpor) != unfair_cycle_exists(naive) ||
        bound_hit(dpor) != bound_hit(naive) ||
        dpor.schedules_explored > naive.schedules_explored) {
        std::cerr << "oracle mismatch\n";
        print_program(program);
        std::cerr << "  naive schedules=" << naive.schedules_explored
                  << " race=" << naive.first_race.has_value()
                  << " deadlock=" << naive.first_deadlock.has_value()
                  << " error=" << naive.first_error.has_value()
                  << " assertion=" << naive.first_assertion.has_value()
                  << " cycle=" << cycle_exists(naive)
                  << " fair_cycle=" << fair_cycle_exists(naive)
                  << " strongly_unfair_cycle="
                  << strongly_unfair_cycle_exists(naive)
                  << " unfair_cycle=" << unfair_cycle_exists(naive)
                  << " bound=" << bound_hit(naive) << '\n';
        std::cerr << "  dpor schedules=" << dpor.schedules_explored
                  << " race=" << dpor.first_race.has_value()
                  << " deadlock=" << dpor.first_deadlock.has_value()
                  << " error=" << dpor.first_error.has_value()
                  << " assertion=" << dpor.first_assertion.has_value()
                  << " cycle=" << cycle_exists(dpor)
                  << " fair_cycle=" << fair_cycle_exists(dpor)
                  << " strongly_unfair_cycle="
                  << strongly_unfair_cycle_exists(dpor)
                  << " unfair_cycle=" << unfair_cycle_exists(dpor)
                  << " bound=" << bound_hit(dpor) << '\n';
        std::abort();
    }

    assert(dpor.first_race.has_value() == naive.first_race.has_value());
    assert(dpor.first_deadlock.has_value() == naive.first_deadlock.has_value());
    assert(dpor.first_error.has_value() == naive.first_error.has_value());
    assert(dpor.first_assertion.has_value() == naive.first_assertion.has_value());
    assert(cycle_exists(dpor) == cycle_exists(naive));
    assert(fair_cycle_exists(dpor) == fair_cycle_exists(naive));
    assert(strongly_unfair_cycle_exists(dpor) ==
           strongly_unfair_cycle_exists(naive));
    assert(unfair_cycle_exists(dpor) == unfair_cycle_exists(naive));
    assert(bound_hit(dpor) == bound_hit(naive));
    assert(dpor.schedules_explored <= naive.schedules_explored);
    assert_replays_dpor_report(checker, dpor);

    ++programs_checked;
    naive_total += naive.schedules_explored;
    dpor_total += dpor.schedules_explored;
    cycle_counts.naive_fair += naive.fair_cycles;
    cycle_counts.dpor_fair += dpor.fair_cycles;
    cycle_counts.naive_strongly_unfair += naive.strongly_unfair_cycles;
    cycle_counts.dpor_strongly_unfair += dpor.strongly_unfair_cycles;
    cycle_counts.naive_unfair += naive.unfair_cycles;
    cycle_counts.dpor_unfair += dpor.unfair_cycles;
}

void assert_strict_reduction(const model::Program& program) {
    const model::ModelChecker checker(program);
    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    assert(dpor.schedules_explored < naive.schedules_explored);
}

void assert_exact_dpor_schedules(const model::Program& program, std::size_t expected) {
    const model::ModelChecker checker(program);
    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    assert(dpor.schedules_explored < naive.schedules_explored);
    if (dpor.schedules_explored != expected) {
        std::cerr << "DPOR exact fixture changed: expected=" << expected
                  << " got=" << dpor.schedules_explored
                  << " naive=" << naive.schedules_explored << '\n';
        print_program(program);
        std::abort();
    }
}

void assert_dpor_schedules_at_most(const model::Program& program, std::size_t upper_bound) {
    const model::ModelChecker checker(program);
    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    if (dpor.schedules_explored > upper_bound) {
        std::cerr << "DPOR schedule bound exceeded: expected <= " << upper_bound
                  << " got " << dpor.schedules_explored
                  << " naive=" << naive.schedules_explored << '\n';
        print_program(program);
        std::abort();
    }
    if (dpor.schedules_explored >= naive.schedules_explored ||
        dpor.first_race.has_value() != naive.first_race.has_value() ||
        dpor.first_deadlock.has_value() != naive.first_deadlock.has_value() ||
        dpor.first_error.has_value() != naive.first_error.has_value() ||
        dpor.first_assertion.has_value() != naive.first_assertion.has_value() ||
        cycle_exists(dpor) != cycle_exists(naive) ||
        fair_cycle_exists(dpor) != fair_cycle_exists(naive) ||
        strongly_unfair_cycle_exists(dpor) !=
            strongly_unfair_cycle_exists(naive) ||
        unfair_cycle_exists(dpor) != unfair_cycle_exists(naive) ||
        bound_hit(dpor) != bound_hit(naive)) {
        std::cerr << "DPOR upper-bound fixture changed verdict or lost reduction\n";
        print_program(program);
        std::abort();
    }
    assert(dpor.schedules_explored < naive.schedules_explored);
    assert(dpor.schedules_explored <= upper_bound);
    assert(dpor.first_race.has_value() == naive.first_race.has_value());
    assert(dpor.first_deadlock.has_value() == naive.first_deadlock.has_value());
    assert(dpor.first_error.has_value() == naive.first_error.has_value());
    assert(dpor.first_assertion.has_value() == naive.first_assertion.has_value());
    assert(cycle_exists(dpor) == cycle_exists(naive));
    assert(fair_cycle_exists(dpor) == fair_cycle_exists(naive));
    assert(strongly_unfair_cycle_exists(dpor) ==
           strongly_unfair_cycle_exists(naive));
    assert(unfair_cycle_exists(dpor) == unfair_cycle_exists(naive));
    assert(bound_hit(dpor) == bound_hit(naive));
    assert_replays_dpor_report(checker, dpor);
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
        model::Program{{
            {lock("m"), wait("cv", "m"), unlock("m")},
            {signal("cv")},
        }},
        model::Program{{
            {write("x")},
            {join(0), write("x")},
        }},
        model::Program{{
            {write("x"), spawn(1), join(1)},
            {write("x")},
        }},
        model::Program{{
            {spawn(1), write("x")},
            {write("x")},
        }},
        model::Program{{
            {lock("m"), wait("cv", "m"), read("x"), unlock("m")},
            {lock("m"), signal("cv"), unlock("m"), write("x")},
        }},
        model::Program{{
            {wlock("rw"), write("x"), wunlock("rw")},
            {rlock("rw"), read("x"), runlock("rw")},
        }},
        // Semaphores start with zero permits, so an unseeded wait is a
        // replayable semaphore-tagged deadlock rather than clean completion.
        model::Program{{
            {sem_wait("sem")},
            {yield()},
        }},
        // A successful wait acquires the accumulated post-release frontier;
        // the poster's plain publication must therefore be race-free.
        model::Program{{
            {write("published"), sem_post("sem")},
            {sem_wait("sem"), read("published")},
        }},
        // A cyclic fixture makes the oracle exercise cycle-existence and
        // identical lasso replay rather than only agreeing on its absence.
        // Before the peer yields the spin witness is unfair; after the peer
        // finishes the same spinner supplies a fair-divergence witness, so both
        // class-existence gates are non-vacuous.
        model::Program{{
            {set(1, 1), label("spin"), bnz(1, "spin")},
            {yield()},
        }},
        // Thread 1's Lock(m) endpoint blinks on only between thread 0's
        // release and reacquisition. This makes the strong-class existence
        // comparison non-vacuous while remaining weakly fair.
        model::Program{{
            {set(7, 1), lock("m"), label("retry"), unlock("m"),
             lock("m"), bnz(7, "retry")},
            {lock("m")},
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
    require(kActions.size() == 25,
            "two-thread oracle alphabet must include TryLock");
    const model::Action& try_lock_entry = kActions.at(8);
    require(try_lock_entry.kind == model::ActionKind::TryLock &&
                try_lock_entry.mutex == "m" &&
                try_lock_entry.destination.has_value() &&
                *try_lock_entry.destination == 0,
            "two-thread oracle TryLock entry must be try_lock m -> r0");
    std::size_t programs_checked = 0;
    std::size_t naive_total = 0;
    std::size_t dpor_total = 0;
    CycleCounts cycle_counts;

    // Deterministically enumerates two-thread programs with 0..3 actions per
    // thread over kActions, including atomic acquire/release/RMW operations,
    // nonblocking mutex acquisition,
    // Spawn, Join, Mesa condition-variable actions, all four reader-writer
    // lock actions, counting-semaphore post/wait actions, and a two-party
    // cyclic-barrier arrival. Each length pair
    // is capped
    // at 2048 programs; larger length pairs use evenly spaced encoded indexes,
    // not randomness.
    constexpr std::uint64_t kProgramsPerLengthPairCap = 2048;
    for (std::size_t lhs_length = 0; lhs_length <= 3; ++lhs_length) {
        for (std::size_t rhs_length = 0; rhs_length <= 3; ++rhs_length) {
            const auto count = pow_actions(lhs_length + rhs_length);
            const auto samples = std::min<std::uint64_t>(count, kProgramsPerLengthPairCap);
            for (std::uint64_t sample = 0; sample < samples; ++sample) {
                const auto encoded = count == samples ? sample : (sample * count) / samples;
                cross_validate_program(
                    two_thread_program(encoded, lhs_length, rhs_length),
                    programs_checked,
                    naive_total,
                    dpor_total,
                    cycle_counts);
            }
        }
    }

    for (const auto& program : hand_picked_programs()) {
        cross_validate_program(
            program, programs_checked, naive_total, dpor_total, cycle_counts);
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
    assert_exact_dpor_schedules(model::Program{{
        {yield(), yield(), lock("m"), unlock("m")},
        {yield(), yield(), lock("m"), unlock("m")},
    }}, 3);
    assert_exact_dpor_schedules(model::Program{{
        {write("a"), write("b"), lock("m"), unlock("m")},
        {write("c"), write("d"), lock("m"), unlock("m")},
    }}, 3);
    // The only semantic enabler for thread 0's Join(1) is thread 1 reaching
    // completion; threads 2 and 3 perform only Yield actions, so they cannot
    // change Join enabledness or expose a distinct modeled bug. There is one
    // hand-counted trace class for "thread 1's three Yields, then Join"; the
    // small upper bound rejects both prefix-wide all-enabled disabled repairs
    // and treating an enabled valid Join as dependent with unrelated Yields.
    assert_dpor_schedules_at_most(model::Program{{
        {join(1)},
        {yield(), yield(), yield()},
        {yield(), yield(), yield()},
        {yield(), yield(), yield()},
    }}, 4);
    // Two joiners wait for the same target. The hand-counted semantic chain is
    // still just thread 1's three Yields before the two Join(1) actions; the
    // fourth thread's Yields neither finish the target nor change any modeled
    // verdict. The bound catches the old behavior where every disabled Join
    // repair repeatedly added that unrelated worker at dependent prefixes, and
    // where enabled valid Joins stayed dependent with unrelated Yields.
    assert_dpor_schedules_at_most(model::Program{{
        {join(1)},
        {yield(), yield(), yield()},
        {join(1)},
        {yield(), yield(), yield()},
    }}, 4);

    require(cycle_counts.naive_fair > 0 && cycle_counts.dpor_fair > 0,
            "two-thread oracle fair-cycle gate is vacuous");
    require(cycle_counts.naive_strongly_unfair > 0 &&
                cycle_counts.dpor_strongly_unfair > 0,
            "two-thread oracle strongly-unfair-cycle gate is vacuous");
    require(cycle_counts.naive_unfair > 0 && cycle_counts.dpor_unfair > 0,
            "two-thread oracle unfair-cycle gate is vacuous");

    std::cout << "dpor_oracle: programs checked=" << programs_checked
              << " alphabet=" << kActions.size()
              << " cap_per_length_pair=" << kProgramsPerLengthPairCap
              << " naive schedules total=" << naive_total
              << " dpor schedules total=" << dpor_total
              << " naive_fair_cycles=" << cycle_counts.naive_fair
              << " dpor_fair_cycles=" << cycle_counts.dpor_fair
              << " naive_strongly_unfair_cycles="
              << cycle_counts.naive_strongly_unfair
              << " dpor_strongly_unfair_cycles="
              << cycle_counts.dpor_strongly_unfair
              << " naive_unfair_cycles=" << cycle_counts.naive_unfair
              << " dpor_unfair_cycles=" << cycle_counts.dpor_unfair << '\n';
#ifdef DPOR_SYMMETRY_DIAGNOSIS
    symmetry_diagnosis::print_summaries(std::cout);
#endif
    return 0;
}
