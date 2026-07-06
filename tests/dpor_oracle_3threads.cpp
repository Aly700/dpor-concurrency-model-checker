// Independent 3-thread adversarial sweep: deterministically sample programs
// with 3 threads x 2 actions over memory, atomic acquire/release/RMW, mutex,
// Join, and Mesa condition-variable actions. The full 13^6 family is capped
// at 65536 evenly spaced encoded indexes, not randomness. Each sampled program
// asserts naive/DPOR
// verdict equality, schedule dominance, and replay identity of every DPOR
// report. This targets the disabled-transition backtrack fallback, which
// 2-thread sweeps exercise only weakly.
#include "model/checker.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
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

Action join(ThreadId target) {
    Action action;
    action.kind = ActionKind::Join;
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

void assert_replays_dpor_report(const ModelChecker& checker, const CheckResult& dpor) {
    if (dpor.first_race) {
        const auto r = checker.replay(dpor.first_race->schedule);
        assert(r.first_race && *r.first_race == *dpor.first_race);
    }
    if (dpor.first_deadlock) {
        const auto r = checker.replay(dpor.first_deadlock->schedule);
        assert(r.first_deadlock && *r.first_deadlock == *dpor.first_deadlock);
    }
    if (dpor.first_error) {
        const auto r = checker.replay(dpor.first_error->schedule);
        assert(r.first_error && *r.first_error == *dpor.first_error);
    }
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
        dpor.schedules_explored > naive.schedules_explored) {
        std::cerr << "MISMATCH in 3-thread sweep at checked index " << checked << "\n";
        assert(false && "3-thread oracle mismatch");
    }

    assert(dpor.first_race.has_value() == naive.first_race.has_value());
    assert(dpor.first_deadlock.has_value() == naive.first_deadlock.has_value());
    assert(dpor.first_error.has_value() == naive.first_error.has_value());
    assert(dpor.schedules_explored <= naive.schedules_explored);
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
        join(0),
        join(1),
        join(2),
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
            {lock("m"), wait("cv", "m"), read("x")},
            {lock("m"), signal("cv")},
            {write("x"), broadcast("cv")},
        }},
    };
    for (const auto& program : handpicked) {
        cross_validate_program(program, checked, naive_total, dpor_total, strict);
    }

    std::cout << "3-thread sweep: programs=" << checked
              << " alphabet=" << k
              << " action_slots=6"
              << " cap=" << kProgramsCap
              << " naive_schedules=" << naive_total
              << " dpor_schedules=" << dpor_total
              << " strict_reductions=" << strict << "\n";
    return 0;
}
