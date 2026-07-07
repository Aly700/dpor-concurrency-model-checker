#include "model/action.hpp"
#include "model/checker.hpp"

#include <cassert>
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

model::Action atomic_rmw(std::string address) {
    model::Action action;
    action.kind = model::ActionKind::AtomicRmw;
    action.address = std::move(address);
    return action;
}

void assert_naive_dpor_agree(const model::Program& program) {
    const model::ModelChecker checker(program);
    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();

    assert(naive.first_race.has_value() == dpor.first_race.has_value());
    assert(naive.first_deadlock.has_value() == dpor.first_deadlock.has_value());
    assert(naive.first_error.has_value() == dpor.first_error.has_value());
    assert(naive.first_assertion.has_value() == dpor.first_assertion.has_value());
    assert((naive.bound_exceeded_executions > 0) == (dpor.bound_exceeded_executions > 0));
    assert(dpor.schedules_explored <= naive.schedules_explored);

    if (dpor.first_race.has_value()) {
        const auto replay = checker.replay(dpor.first_race->schedule);
        assert(replay.first_race.has_value());
        assert(*replay.first_race == *dpor.first_race);
    }
}

void message_passing_has_unordered_race_but_ordered_replay_is_clean() {
    const model::Program program{{
        {write("x"), atomic_store("f")},
        {atomic_load("f"), read("x")},
    }};
    const model::ModelChecker checker(program);

    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    assert(naive.first_race.has_value());
    assert(dpor.first_race.has_value());
    assert_naive_dpor_agree(program);

    const auto ordered = checker.replay({{0, 0}, {0, 1}, {1, 0}, {1, 1}});
    assert(!ordered.first_race.has_value());
    assert(!ordered.first_deadlock.has_value());
    assert(!ordered.first_error.has_value());
}

void release_store_replaces_location_clock_instead_of_joining_it() {
    const model::Program program{{
        {write("x"), atomic_store("f")},
        {atomic_store("f")},
        {atomic_load("f"), write("x")},
    }};
    const model::ModelChecker checker(program);

    const auto replay = checker.replay({{0, 0}, {0, 1}, {1, 0}, {2, 0}, {2, 1}});
    assert(replay.first_race.has_value());
    assert(replay.first_race->address == "x");
}

void rmw_continues_release_sequence_for_later_acquire_load() {
    const model::Program program{{
        {write("x"), atomic_store("f")},
        {atomic_rmw("f")},
        {atomic_load("f"), read("x")},
    }};
    const model::ModelChecker checker(program);

    const auto replay = checker.replay({{0, 0}, {0, 1}, {1, 0}, {2, 0}, {2, 1}});
    assert(!replay.first_race.has_value());
    assert(!replay.first_deadlock.has_value());
    assert(!replay.first_error.has_value());
}

void atomic_atomic_same_address_is_never_a_race() {
    const model::Program program{{
        {atomic_store("f"), atomic_rmw("f")},
        {atomic_load("f"), atomic_store("f"), atomic_rmw("f")},
    }};
    const model::ModelChecker checker(program);

    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    assert(!naive.first_race.has_value());
    assert(!dpor.first_race.has_value());
    assert_naive_dpor_agree(program);
}

void mixed_plain_atomic_same_address_reports_race_when_unordered() {
    {
        const model::Program program{{
            {write("x")},
            {atomic_load("x")},
        }};
        const model::ModelChecker checker(program);
        const auto naive = checker.explore_naive();
        const auto dpor = checker.explore_dpor();
        assert(naive.first_race.has_value());
        assert(dpor.first_race.has_value());
        assert_naive_dpor_agree(program);
    }
    {
        const model::Program program{{
            {read("x")},
            {atomic_store("x")},
        }};
        const model::ModelChecker checker(program);
        const auto naive = checker.explore_naive();
        const auto dpor = checker.explore_dpor();
        assert(naive.first_race.has_value());
        assert(dpor.first_race.has_value());
        assert_naive_dpor_agree(program);
    }
}

void atomic_independence_clauses_match_clock_and_race_semantics() {
    assert(model::independent(atomic_load("f"), atomic_load("f")));
    assert(!model::independent(atomic_load("f"), atomic_store("f")));
    assert(!model::independent(atomic_store("f"), atomic_rmw("f")));
    assert(!model::independent(atomic_rmw("f"), atomic_load("f")));
    assert(!model::independent(atomic_load("x"), read("x")));
    assert(!model::independent(atomic_store("x"), write("x")));
    assert(model::independent(atomic_store("f"), write("x")));
}

} // namespace

int main() {
    message_passing_has_unordered_race_but_ordered_replay_is_clean();
    release_store_replaces_location_clock_instead_of_joining_it();
    rmw_continues_release_sequence_for_later_acquire_load();
    atomic_atomic_same_address_is_never_a_race();
    mixed_plain_atomic_same_address_reports_race_when_unordered();
    atomic_independence_clauses_match_clock_and_race_semantics();
    return 0;
}
