// Independent 3-thread adversarial sweep: deterministically sample programs
// with 3 threads x 2 actions over memory, atomic acquire/release/RMW, mutex,
// reader-writer lock, counting semaphore, Spawn, Join, and Mesa
// condition-variable actions. The full alphabet^6 family is capped
// at 65536 evenly spaced encoded indexes, not randomness. Each sampled program
// enforces naive/DPOR verdict equality, schedule dominance, and replay identity
// of every DPOR report. This targets the disabled-transition backtrack
// fallback, which 2-thread sweeps exercise only weakly.
#include "model/checker.hpp"

#include <algorithm>
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

void cross_validate_program(const Program& p,
                            std::size_t& checked,
                            std::size_t& naive_total,
                            std::size_t& dpor_total,
                            std::size_t& strict) {
    const ModelChecker checker(p);
    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();

    if (dpor.first_race.has_value() != naive.first_race.has_value() ||
        dpor.first_deadlock.has_value() != naive.first_deadlock.has_value() ||
        dpor.first_error.has_value() != naive.first_error.has_value() ||
        dpor.first_assertion.has_value() != naive.first_assertion.has_value() ||
        cycle_exists(dpor) != cycle_exists(naive) ||
        bound_hit(dpor) != bound_hit(naive) ||
        dpor.schedules_explored > naive.schedules_explored) {
        std::cerr << "MISMATCH in 3-thread sweep at checked index " << checked << "\n";
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
        sem_post("sem"),
        sem_wait("sem"),
    };
    const std::size_t k = alphabet.size();

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
        // SemPost/SemPost may commute, but a zero-permit waiter becomes
        // enabled after either first post. When the second poster enables the
        // waiter first, it can read x before the first poster publishes and
        // expose a race, pinning the waiter-between-posts middle witness.
        Program{{
            {write("x"), sem_post("sem")},
            {sem_post("sem")},
            {sem_wait("sem"), read("x")},
        }},
    };
    for (const auto& program : handpicked) {
        cross_validate_program(program, checked, naive_total, dpor_total, strict);
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
    return 0;
}
