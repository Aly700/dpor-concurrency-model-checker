// Independent 3-thread adversarial sweep: deterministically sample programs
// with 3 threads x 2 actions over memory, atomic acquire/release/RMW, mutex
// lock/try-lock/unlock,
// reader-writer lock, counting semaphore, Spawn, Join, and Mesa
// condition-variable actions. The full alphabet^6 family is capped
// at 65536 evenly spaced encoded indexes, not randomness. Each sampled program
// enforces naive/DPOR verdict equality, schedule dominance, and replay identity
// of every DPOR report. This targets the disabled-transition backtrack
// fallback, which 2-thread sweeps exercise only weakly.
#include "model/checker.hpp"

#ifdef DPOR_SYMMETRY_DIAGNOSIS
#include "symmetry_diagnosis.hpp"

#include <chrono>
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace model;

namespace {

Action read(std::string address) {
    Action action;
    action.kind = ActionKind::Read;
    action.address = std::move(address);
    return action;
}

Action write(std::string address) {
    Action action;
    action.kind = ActionKind::Write;
    action.address = std::move(address);
    return action;
}

Action atomic_load(std::string address) {
    Action action;
    action.kind = ActionKind::AtomicLoad;
    action.address = std::move(address);
    return action;
}

Action atomic_store(std::string address) {
    Action action;
    action.kind = ActionKind::AtomicStore;
    action.address = std::move(address);
    return action;
}

Action atomic_rmw(std::string address) {
    Action action;
    action.kind = ActionKind::AtomicRmw;
    action.address = std::move(address);
    return action;
}

Action lock(std::string mutex) {
    Action action;
    action.kind = ActionKind::Lock;
    action.mutex = std::move(mutex);
    return action;
}

Action try_lock(std::string mutex, RegisterId destination = 0) {
    Action action;
    action.kind = ActionKind::TryLock;
    action.mutex = std::move(mutex);
    action.destination = destination;
    return action;
}

Action unlock(std::string mutex) {
    Action action;
    action.kind = ActionKind::Unlock;
    action.mutex = std::move(mutex);
    return action;
}

Action rwlock_action(ActionKind kind, std::string rwlock) {
    Action action;
    action.kind = kind;
    action.rwlock = std::move(rwlock);
    return action;
}

Action rlock(std::string rwlock) {
    return rwlock_action(ActionKind::RLock, std::move(rwlock));
}

Action runlock(std::string rwlock) {
    return rwlock_action(ActionKind::RUnlock, std::move(rwlock));
}

Action wlock(std::string rwlock) {
    return rwlock_action(ActionKind::WLock, std::move(rwlock));
}

Action wunlock(std::string rwlock) {
    return rwlock_action(ActionKind::WUnlock, std::move(rwlock));
}

Action upgrade(std::string rwlock) {
    return rwlock_action(ActionKind::Upgrade, std::move(rwlock));
}

Action downgrade(std::string rwlock) {
    return rwlock_action(ActionKind::Downgrade, std::move(rwlock));
}

Action semaphore_action(ActionKind kind, std::string semaphore) {
    Action action;
    action.kind = kind;
    action.semaphore = std::move(semaphore);
    return action;
}

Action sem_post(std::string semaphore) {
    return semaphore_action(ActionKind::SemPost, std::move(semaphore));
}

Action sem_wait(std::string semaphore) {
    return semaphore_action(ActionKind::SemWait, std::move(semaphore));
}

Action barrier_wait(std::string barrier, std::uint32_t parties) {
    Action action;
    action.kind = ActionKind::BarrierWait;
    action.barrier = std::move(barrier);
    action.parties = parties;
    return action;
}

Action join(ThreadId target) {
    Action action;
    action.kind = ActionKind::Join;
    action.target = target;
    return action;
}

Action spawn(ThreadId target) {
    Action action;
    action.kind = ActionKind::Spawn;
    action.target = target;
    return action;
}

Action wait(std::string condition, std::string mutex) {
    Action action;
    action.kind = ActionKind::Wait;
    action.condition = std::move(condition);
    action.mutex = std::move(mutex);
    return action;
}

Action signal(std::string condition) {
    Action action;
    action.kind = ActionKind::Signal;
    action.condition = std::move(condition);
    return action;
}

Action broadcast(std::string condition) {
    Action action;
    action.kind = ActionKind::Broadcast;
    action.condition = std::move(condition);
    return action;
}

ValueOperand imm(Value value) {
    ValueOperand operand;
    operand.kind = ValueOperandKind::Immediate;
    operand.immediate = value;
    return operand;
}

Action set(RegisterId reg, Value value) {
    Action action;
    action.kind = ActionKind::Set;
    action.destination = reg;
    action.value = imm(value);
    return action;
}

Action label(std::string name) {
    Action action;
    action.kind = ActionKind::Label;
    action.label = std::move(name);
    return action;
}

Action bnz(RegisterId reg, std::string target) {
    Action action;
    action.kind = ActionKind::BranchNonzero;
    action.source_register = reg;
    action.label = std::move(target);
    return action;
}

Action yield() {
    Action action;
    action.kind = ActionKind::Yield;
    return action;
}

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void assert_replays_dpor_report(const ModelChecker& checker, const CheckResult& dpor) {
    if (dpor.first_race) {
        const auto r = checker.replay(dpor.first_race->schedule);
        require(r.first_race && *r.first_race == *dpor.first_race,
                "3-thread race report did not replay identically");
    }
    if (dpor.first_deadlock) {
        const auto r = checker.replay(dpor.first_deadlock->schedule);
        require(r.first_deadlock && *r.first_deadlock == *dpor.first_deadlock,
                "3-thread deadlock report did not replay identically");
    }
    if (dpor.first_error) {
        const auto r = checker.replay(dpor.first_error->schedule);
        require(r.first_error && *r.first_error == *dpor.first_error,
                "3-thread error report did not replay identically");
    }
    if (dpor.first_assertion) {
        const auto r = checker.replay(dpor.first_assertion->schedule);
        require(r.first_assertion && *r.first_assertion == *dpor.first_assertion,
                "3-thread assertion report did not replay identically");
    }
    if (dpor.first_nontermination) {
        const auto r = checker.replay(dpor.first_nontermination->schedule);
        require(r.first_nontermination &&
                    *r.first_nontermination == *dpor.first_nontermination,
                "3-thread nontermination report did not replay identically");
    }
}

bool bound_hit(const CheckResult& result) {
    return result.bound_exceeded_executions > 0;
}

bool cycle_exists(const CheckResult& result) {
    return result.cycles_detected > 0;
}

bool fair_cycle_exists(const CheckResult& result) {
    return result.fair_cycles > 0;
}

bool strongly_unfair_cycle_exists(const CheckResult& result) {
    return result.strongly_unfair_cycles > 0;
}

bool unfair_cycle_exists(const CheckResult& result) {
    return result.unfair_cycles > 0;
}

std::string action_string(const Action& action) {
    switch (action.kind) {
    case ActionKind::RLock:
        return "RLock " + action.rwlock;
    case ActionKind::RUnlock:
        return "RUnlock " + action.rwlock;
    case ActionKind::WLock:
        return "WLock " + action.rwlock;
    case ActionKind::WUnlock:
        return "WUnlock " + action.rwlock;
    case ActionKind::Upgrade:
        return "Upgrade " + action.rwlock;
    case ActionKind::Downgrade:
        return "Downgrade " + action.rwlock;
    default:
        return std::to_string(static_cast<int>(action.kind));
    }
}

void print_program(const Program& program) {
    for (std::size_t tid = 0; tid < program.threads.size(); ++tid) {
        std::cerr << "  t" << tid << ':';
        for (const Action& action : program.threads.at(tid)) {
            std::cerr << " [" << action_string(action) << ']';
        }
        std::cerr << '\n';
    }
}

void cross_validate_program(const Program& p,
                            std::size_t& checked,
                            std::size_t& naive_total,
                            std::size_t& dpor_total,
                            std::size_t& strict) {
    const ModelChecker checker(p);
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
        "dpor_oracle_3threads",
        {"memory=sc", "paired", "threads=3"},
        p,
        checker,
        MemoryModel::SC,
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
        std::cerr << "MISMATCH in 3-thread sweep at checked index " << checked << "\n";
        print_program(p);
        throw std::runtime_error("3-thread oracle mismatch");
    }
    assert_replays_dpor_report(checker, dpor);

    ++checked;
    naive_total += naive.schedules_explored;
    dpor_total += dpor.schedules_explored;
    if (dpor.schedules_explored < naive.schedules_explored) {
        ++strict;
    }
}

} // namespace

int main() {
    const std::vector<Action> alphabet = {
        read("x"),
        write("x"),
        atomic_load("f"),
        atomic_store("f"),
        atomic_rmw("f"),
        lock("m"),
        try_lock("m"),
        unlock("m"),
        wait("cv", "m"),
        signal("cv"),
        broadcast("cv"),
        spawn(1),
        spawn(2),
        join(0),
        join(1),
        join(2),
        rlock("rw"),
        runlock("rw"),
        wlock("rw"),
        wunlock("rw"),
        upgrade("rw"),
        downgrade("rw"),
        sem_post("sem"),
        sem_wait("sem"),
        barrier_wait("bar", 3),
    };
    const std::size_t k = alphabet.size();
    require(k == 25,
            "three-thread oracle alphabet must include TryLock and conversions");
    const Action& try_lock_entry = alphabet.at(6);
    require(try_lock_entry.kind == ActionKind::TryLock &&
                try_lock_entry.mutex == "m" &&
                try_lock_entry.destination.has_value() &&
                *try_lock_entry.destination == 0,
            "three-thread oracle TryLock entry must be try_lock m -> r0");
    const std::array<ActionKind, 3> condition_kinds{
        ActionKind::Wait,
        ActionKind::Signal,
        ActionKind::Broadcast,
    };
    for (std::size_t i = 0; i < condition_kinds.size(); ++i) {
        const Action& action = alphabet.at(8 + i);
        require(action.kind == condition_kinds.at(i) &&
                    action.condition == "cv" &&
                    (action.kind != ActionKind::Wait || action.mutex == "m"),
                "three-thread oracle must contain Wait/Signal/Broadcast on cv");
    }
    const std::array<ActionKind, 6> rwlock_kinds{
        ActionKind::RLock,
        ActionKind::RUnlock,
        ActionKind::WLock,
        ActionKind::WUnlock,
        ActionKind::Upgrade,
        ActionKind::Downgrade,
    };
    for (std::size_t i = 0; i < rwlock_kinds.size(); ++i) {
        const Action& action = alphabet.at(16 + i);
        require(action.kind == rwlock_kinds.at(i) && action.rwlock == "rw",
                "three-thread oracle must contain all six rwlock operations");
    }

    std::size_t checked = 0, naive_total = 0, dpor_total = 0, strict = 0;
    std::size_t combos = 1;
    for (int i = 0; i < 6; ++i) combos *= k;

    constexpr std::size_t kProgramsCap = 65536;
    const std::size_t samples = std::min(combos, kProgramsCap);
    for (std::size_t sample = 0; sample < samples; ++sample) {
        const std::size_t code = combos == samples ? sample : (sample * combos) / samples;
        std::size_t c = code;
        Program p;
        p.threads.assign(3, {});
        for (int t = 0; t < 3; ++t) {
            for (int a = 0; a < 2; ++a) {
                p.threads[t].push_back(alphabet[c % k]);
                c /= k;
            }
        }

        cross_validate_program(p, checked, naive_total, dpor_total, strict);
    }

    const Program weak_and_fair_cycles{{
        {set(7, 1), label("spin"), bnz(7, "spin")},
        {yield()},
        {},
    }};
    const Program strongly_unfair_cycle{{
        {set(7, 1), lock("m"), label("retry"), unlock("m"),
         lock("m"), bnz(7, "retry")},
        {lock("m")},
        {},
    }};
    const std::vector<Program> handpicked = {
        Program{{
            {lock("m"), wait("cv", "m")},
            {lock("m"), wait("cv", "m")},
            {broadcast("cv")},
        }},
        Program{{
            {join(1)},
            {join(0)},
            {join(0)},
        }},
        Program{{
            {write("x")},
            {join(0), write("x")},
            {write("y"), join(1)},
        }},
        Program{{
            {spawn(1), join(1)},
            {write("x")},
            {write("x")},
        }},
        Program{{
            {write("x"), spawn(1)},
            {write("x")},
            {join(1)},
        }},
        Program{{
            {lock("m"), wait("cv", "m"), read("x")},
            {lock("m"), signal("cv")},
            {write("x"), broadcast("cv")},
        }},
        Program{{
            {rlock("rw"), read("x"), runlock("rw")},
            {rlock("rw"), read("x"), runlock("rw")},
            {rlock("rw"), read("x"), runlock("rw")},
        }},
        // Exercise both conversions on a valid ownership path while two
        // independent readers can transiently delay Upgrade.
        Program{{
            {rlock("rw"), read("x"), runlock("rw")},
            {rlock("rw"), upgrade("rw"), write("x"),
             downgrade("rw"), runlock("rw")},
            {rlock("rw"), read("x"), runlock("rw")},
        }},
        // SemPost/SemPost may commute, but a zero-permit waiter becomes
        // enabled after either first post. When the second poster enables the
        // waiter first, it can read x before the first poster publishes and
        // expose a race, pinning the waiter-between-posts middle witness.
        Program{{
            {write("x"), sem_post("sem")},
            {sem_post("sem")},
            {sem_wait("sem"), read("x")},
        }},
        // Before thread 1 yields, thread 0's spin cycle is weakly unfair;
        // after it finishes, the same loop supplies a genuinely fair cycle.
        weak_and_fair_cycles,
        // Thread 1's Lock(m) is enabled only between release and reacquire,
        // making the strong-class existence comparison non-vacuous.
        strongly_unfair_cycle,
    };
    for (const auto& program : handpicked) {
        cross_validate_program(program, checked, naive_total, dpor_total, strict);
    }

    for (const CheckResult& result :
         {ModelChecker(weak_and_fair_cycles).explore_naive(),
          ModelChecker(weak_and_fair_cycles).explore_dpor()}) {
        require(fair_cycle_exists(result) && unfair_cycle_exists(result),
                "three-thread weak/fair cycle gate is vacuous");
    }
    for (const CheckResult& result :
         {ModelChecker(strongly_unfair_cycle).explore_naive(),
          ModelChecker(strongly_unfair_cycle).explore_dpor()}) {
        require(strongly_unfair_cycle_exists(result),
                "three-thread strongly-unfair cycle gate is vacuous");
    }

    // Three fixed per-thread chains have 9!/(3!^3) = 1680 interleavings.
    // With no writer action anywhere in this program, every cross-thread
    // reader-mode pair commutes, so the reader-only DPOR refinement must retain
    // exactly one representative.
    const Program three_readers{{
        {rlock("rw"), read("x"), runlock("rw")},
        {rlock("rw"), read("x"), runlock("rw")},
        {rlock("rw"), read("x"), runlock("rw")},
    }};
    const ModelChecker reader_checker(three_readers);
    const CheckResult reader_naive = reader_checker.explore_naive();
    const CheckResult reader_dpor = reader_checker.explore_dpor();
    require(reader_naive.schedules_explored == 1680,
            "three-reader naive schedule count changed");
    require(reader_dpor.schedules_explored == 1,
            "three-reader DPOR schedule count changed");

    std::cout << "3-thread sweep: programs=" << checked
              << " alphabet=" << k
              << " action_slots=6"
              << " cap=" << kProgramsCap
              << " naive_schedules=" << naive_total
              << " dpor_schedules=" << dpor_total
              << " strict_reductions=" << strict << "\n";
#ifdef DPOR_SYMMETRY_DIAGNOSIS
    symmetry_diagnosis::print_summaries(std::cout);
#endif
    return 0;
}
