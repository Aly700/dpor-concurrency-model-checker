// Independent 3-thread adversarial sweep: enumerate every program with 3
// threads x 2 actions over {Read x, Write x, Lock m, Unlock m, Lock n,
// Unlock n} (6^6 = 46,656 programs) and assert naive/DPOR verdict equality,
// schedule dominance, and replay identity of every DPOR report. This targets
// the disabled-transition backtrack fallback, which 2-thread sweeps exercise
// only weakly.
#include "model/checker.hpp"

#include <cassert>
#include <iostream>
#include <vector>

using namespace model;

int main() {
    const std::vector<Action> alphabet = {
        {ActionKind::Read, "x", ""},  {ActionKind::Write, "x", ""},
        {ActionKind::Lock, "", "m"},  {ActionKind::Unlock, "", "m"},
        {ActionKind::Lock, "", "n"},  {ActionKind::Unlock, "", "n"},
    };
    const std::size_t k = alphabet.size();

    std::size_t checked = 0, naive_total = 0, dpor_total = 0, strict = 0;
    std::size_t combos = 1;
    for (int i = 0; i < 6; ++i) combos *= k;

    for (std::size_t code = 0; code < combos; ++code) {
        std::size_t c = code;
        Program p;
        p.threads.assign(3, {});
        for (int t = 0; t < 3; ++t) {
            for (int a = 0; a < 2; ++a) {
                p.threads[t].push_back(alphabet[c % k]);
                c /= k;
            }
        }

        const ModelChecker checker(p);
        const auto naive = checker.explore_naive();
        const auto dpor = checker.explore_dpor();

        if (dpor.first_race.has_value() != naive.first_race.has_value() ||
            dpor.first_deadlock.has_value() != naive.first_deadlock.has_value() ||
            dpor.first_error.has_value() != naive.first_error.has_value() ||
            dpor.schedules_explored > naive.schedules_explored) {
            std::cerr << "MISMATCH at code " << code << "\n";
            return 1;
        }
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

        ++checked;
        naive_total += naive.schedules_explored;
        dpor_total += dpor.schedules_explored;
        if (dpor.schedules_explored < naive.schedules_explored) ++strict;
    }

    std::cout << "3-thread sweep: programs=" << checked
              << " naive_schedules=" << naive_total
              << " dpor_schedules=" << dpor_total
              << " strict_reductions=" << strict << "\n";
    return 0;
}
