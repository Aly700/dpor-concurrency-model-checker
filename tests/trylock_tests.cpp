#include "model/checker.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

model::ValueOperand immediate(model::Value value) {
    return model::ValueOperand{
        model::ValueOperandKind::Immediate,
        value,
        0,
    };
}

model::ValueOperand reg(model::RegisterId source) {
    model::ValueOperand operand;
    operand.kind = model::ValueOperandKind::Register;
    operand.reg = source;
    return operand;
}

model::Action try_lock(std::string mutex, model::RegisterId destination) {
    model::Action action;
    action.kind = model::ActionKind::TryLock;
    action.mutex = std::move(mutex);
    action.destination = destination;
    return action;
}

model::Action atomic_store(std::string address, model::Value value) {
    model::Action action;
    action.kind = model::ActionKind::AtomicStore;
    action.address = std::move(address);
    action.value = immediate(value);
    return action;
}

model::Action compare_exchange(std::string address,
                               model::ValueOperand expected,
                               model::ValueOperand desired,
                               model::RegisterId destination) {
    model::Action action;
    action.kind = model::ActionKind::CompareExchange;
    action.address = std::move(address);
    action.expected = expected;
    action.value = desired;
    action.destination = destination;
    return action;
}

model::Action lock(std::string mutex) {
    model::Action action;
    action.kind = model::ActionKind::Lock;
    action.mutex = std::move(mutex);
    return action;
}

model::Action unlock(std::string mutex) {
    model::Action action;
    action.kind = model::ActionKind::Unlock;
    action.mutex = std::move(mutex);
    return action;
}

model::Action wait(std::string condition, std::string mutex) {
    model::Action action;
    action.kind = model::ActionKind::Wait;
    action.condition = std::move(condition);
    action.mutex = std::move(mutex);
    return action;
}

model::Action spawn(model::ThreadId target) {
    model::Action action;
    action.kind = model::ActionKind::Spawn;
    action.target = target;
    return action;
}

model::Action set(model::RegisterId reg, model::Value value) {
    model::Action action;
    action.kind = model::ActionKind::Set;
    action.destination = reg;
    action.value = immediate(value);
    return action;
}

model::Action label(std::string name) {
    model::Action action;
    action.kind = model::ActionKind::Label;
    action.label = std::move(name);
    return action;
}

model::Action branch_nonzero(model::RegisterId reg, std::string target) {
    model::Action action;
    action.kind = model::ActionKind::BranchNonzero;
    action.source_register = reg;
    action.label = std::move(target);
    return action;
}

model::Action assert_nonzero(model::RegisterId reg) {
    model::Action action;
    action.kind = model::ActionKind::Assert;
    action.source_register = reg;
    return action;
}

model::ScheduleStep step(model::ThreadId thread, std::uint32_t action_index) {
    return model::ScheduleStep{thread, action_index, std::nullopt};
}

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void require_clean(const model::CheckResult& result, const char* message) {
    require(!result.first_race.has_value(), message);
    require(!result.first_deadlock.has_value(), message);
    require(!result.first_error.has_value(), message);
    require(!result.first_assertion.has_value(), message);
    require(!result.first_nontermination.has_value(), message);
    require(result.bound_exceeded_executions == 0, message);
    require(!result.exploration_capped, message);
}

void require_naive_dpor_agree(const model::Program& program) {
    const model::ModelChecker checker(program);
    const model::CheckResult naive = checker.explore_naive();
    const model::CheckResult dpor = checker.explore_dpor();
    require(naive.first_race.has_value() == dpor.first_race.has_value(),
            "TryLock naive/DPOR race existence differs");
    require(naive.first_deadlock.has_value() == dpor.first_deadlock.has_value(),
            "TryLock naive/DPOR deadlock existence differs");
    require(naive.first_error.has_value() == dpor.first_error.has_value(),
            "TryLock naive/DPOR error existence differs");
    require(naive.first_assertion.has_value() == dpor.first_assertion.has_value(),
            "TryLock naive/DPOR assertion existence differs");
    require((naive.cycles_detected > 0) == (dpor.cycles_detected > 0),
            "TryLock naive/DPOR cycle existence differs");
    require((naive.fair_cycles > 0) == (dpor.fair_cycles > 0),
            "TryLock naive/DPOR fair-cycle existence differs");
    require((naive.strongly_unfair_cycles > 0) ==
                (dpor.strongly_unfair_cycles > 0),
            "TryLock naive/DPOR strongly-unfair-cycle existence differs");
    require((naive.unfair_cycles > 0) == (dpor.unfair_cycles > 0),
            "TryLock naive/DPOR unfair-cycle existence differs");
    require(dpor.schedules_explored <= naive.schedules_explored,
            "TryLock DPOR explored more schedules than naive");
}

void free_try_lock_succeeds_and_owns_the_mutex() {
    const model::Program program{{{
        atomic_store("exact-one", 1),
        try_lock("m", 0),
        compare_exchange("exact-one", reg(0), immediate(1), 1),
        assert_nonzero(1),
        unlock("m"),
    }}};
    const model::ModelChecker checker(program);

    require_clean(checker.replay(
                      {step(0, 0), step(0, 1), step(0, 2),
                       step(0, 3), step(0, 4)}),
                  "free TryLock did not write exactly one, advance, and acquire ownership");
    require_clean(checker.explore_naive(),
                  "free TryLock was not clean under naive exploration");
    require_clean(checker.explore_dpor(),
                  "free TryLock was not clean under DPOR exploration");
}

void direct_api_try_lock_without_destination_defaults_to_r0() {
    model::Action direct_try_lock;
    direct_try_lock.kind = model::ActionKind::TryLock;
    direct_try_lock.mutex = "m";

    const model::Program program{{{
        atomic_store("exact-one", 1),
        direct_try_lock,
        compare_exchange("exact-one", reg(0), immediate(1), 1),
        unlock("m"),
        assert_nonzero(1),
    }}};
    const model::ModelChecker checker(program);

    require_clean(checker.replay(
                      {step(0, 0), step(0, 1), step(0, 2),
                       step(0, 3), step(0, 4)}),
                  "destination-less direct TryLock did not default to r0 with exact one and ownership");
}

void self_try_lock_fails_without_losing_the_original_ownership() {
    const model::Program program{{{
        try_lock("m", 0),
        assert_nonzero(0),
        set(1, 1),
        try_lock("m", 1),
        branch_nonzero(1, "unexpected-success"),
        unlock("m"),
        set(7, 1),
        branch_nonzero(7, "done"),
        label("unexpected-success"),
        assert_nonzero(7),
        label("done"),
    }}};
    const model::ModelChecker checker(program);
    const model::Schedule schedule{
        step(0, 0), step(0, 1), step(0, 2), step(0, 3),
        step(0, 4), step(0, 5), step(0, 6), step(0, 7),
    };

    require_clean(checker.replay(schedule),
                  "self TryLock did not write zero, advance, and preserve ownership");
    require_clean(checker.explore_naive(),
                  "self TryLock produced a terminal bug under naive exploration");
    require_clean(checker.explore_dpor(),
                  "self TryLock produced a terminal bug under DPOR exploration");
}

void held_try_lock_advances_without_blocking_or_deadlocking() {
    const model::Program program{{
        {lock("m"), unlock("m")},
        {
            set(0, 1),
            try_lock("m", 0),
            branch_nonzero(0, "owned"),
            set(7, 1),
            branch_nonzero(7, "done"),
            label("owned"),
            unlock("m"),
            label("done"),
        },
    }};
    const model::ModelChecker checker(program);

    const model::Schedule failed_while_holder_remains_enabled{
        step(0, 0),
        step(1, 0), step(1, 1), step(1, 2), step(1, 3), step(1, 4),
        step(0, 1),
    };
    require_clean(checker.replay(failed_while_holder_remains_enabled),
                  "held TryLock blocked, returned one, or changed the live owner");
    require_clean(checker.explore_naive(),
                  "TryLock introduced a naive deadlock or ownership error");
    require_clean(checker.explore_dpor(),
                  "TryLock introduced a DPOR deadlock or ownership error");
    require_naive_dpor_agree(program);
}

void failed_try_lock_finishes_cleanly_after_its_holder_has_finished() {
    // Spawn forces the only reachable TryLock to occur after thread 0 has
    // finished while retaining m. A blocking implementation would leave
    // thread 1 unfinished with no enabled transition and report a deadlock.
    const model::Program program{{
        {lock("m"), spawn(1)},
        {
            set(0, 1),
            try_lock("m", 0),
            branch_nonzero(0, "unexpected-success"),
            set(7, 1),
            branch_nonzero(7, "done"),
            label("unexpected-success"),
            assert_nonzero(7),
            label("done"),
        },
    }};
    const model::ModelChecker checker(program);

    require_clean(checker.explore_naive(),
                  "failed TryLock entered a naive mutex blocker set");
    require_clean(checker.explore_dpor(),
                  "failed TryLock entered a DPOR mutex blocker set");
    require_naive_dpor_agree(program);
}

void free_mutex_try_lock_pair_keeps_both_winner_orders() {
    const model::Program program{{
        {try_lock("m", 0)},
        {try_lock("m", 0)},
    }};
    const model::ModelChecker checker(program);
    const model::CheckResult naive = checker.explore_naive();
    const model::CheckResult dpor = checker.explore_dpor();

    require(naive.schedules_explored == 2,
            "free-mutex TryLock guard lost one of the two naive winners");
    require(dpor.schedules_explored == 2,
            "free-mutex TryLocks were commuted despite distinct winners and results");
    require_clean(naive, "free-mutex TryLock guard found a naive bug");
    require_clean(dpor, "free-mutex TryLock guard found a DPOR bug");
}

void owner_that_is_one_trier_keeps_try_locks_dependent() {
    const model::Program program{{
        {lock("m"), try_lock("m", 0)},
        {try_lock("m", 0)},
    }};
    const model::ModelChecker checker(program);
    const model::CheckResult naive = checker.explore_naive();
    const model::CheckResult dpor = checker.explore_dpor();

    require(naive.schedules_explored == 3,
            "trier-owner guard changed the three naive terminal orders");
    require(dpor.schedules_explored == 3,
            "owner-present TryLock rule was applied when the owner was one trier");
    require(naive.first_deadlock.has_value() == dpor.first_deadlock.has_value(),
            "trier-owner guard changed deadlock existence under DPOR");
}

void mirrored_owner_that_is_one_trier_keeps_try_locks_dependent() {
    const model::Program program{{
        {try_lock("m", 0)},
        {lock("m"), try_lock("m", 0)},
    }};
    const model::ModelChecker checker(program);
    const model::CheckResult naive = checker.explore_naive();
    const model::CheckResult dpor = checker.explore_dpor();

    require(naive.schedules_explored == 3,
            "mirrored trier-owner guard changed the three naive terminal orders");
    require(dpor.schedules_explored == 3,
            "owner-present TryLock rule checked only the left-hand trier");
    require(naive.first_deadlock.has_value() == dpor.first_deadlock.has_value(),
            "mirrored trier-owner guard changed deadlock existence under DPOR");
}

void unlock_between_failed_and_successful_try_locks_preserves_unique_assertion() {
    // T2 is initially disabled and T1 spawns it only on TryLock failure. Since
    // T0 owns m until its final action, T2 can exist only after T1 has failed
    // under T0; T2 can reach the assertion only by succeeding after T0's
    // middle Unlock. Assertion existence therefore pins fail -> Unlock ->
    // success rather than merely checking a bug shared with the reverse order.
    const model::Program program{{
        {lock("m"), spawn(1), unlock("m")},
        {
            try_lock("m", 0),
            branch_nonzero(0, "done"),
            spawn(2),
            label("done"),
        },
        {
            try_lock("m", 0),
            branch_nonzero(0, "owned"),
            set(7, 1),
            branch_nonzero(7, "done"),
            label("owned"),
            assert_nonzero(7),
            label("done"),
        },
    }};
    const model::ModelChecker checker(program);
    require_clean(checker.replay({
                      step(0, 0), step(0, 1), step(0, 2),
                      step(1, 0), step(1, 1),
                  }),
                  "unlock-first path spawned T2 or reached its assertion");
    require_clean(checker.replay({
                      step(0, 0), step(0, 1),
                      step(1, 0), step(1, 1), step(1, 2),
                      step(2, 0), step(2, 1), step(2, 2), step(2, 3),
                      step(0, 2),
                  }),
                  "T2 failure before the holder Unlock reached the assertion");
    const model::Schedule desired{
        step(0, 0), step(0, 1),
        step(1, 0), step(1, 1), step(1, 2),
        step(0, 2),
        step(2, 0), step(2, 1), step(2, 5),
    };

    const model::CheckResult replay = checker.replay(desired);
    require(replay.first_assertion.has_value() &&
                replay.first_assertion->endpoint == step(2, 5) &&
                replay.first_assertion->reg == 7 &&
                replay.first_assertion->value == 0 &&
                replay.first_assertion->schedule == desired,
            "spawn-gated fail/Unlock/success assertion did not replay exactly");
    const model::CheckResult reproduced =
        checker.replay(replay.first_assertion->schedule);
    require(reproduced.first_assertion.has_value() &&
                *reproduced.first_assertion == *replay.first_assertion,
            "spawn-gated middle-Unlock assertion did not replay identically");

    const model::CheckResult naive = checker.explore_naive();
    const model::CheckResult dpor = checker.explore_dpor();
    require(naive.first_assertion.has_value() && dpor.first_assertion.has_value(),
            "naive/DPOR exploration missed the spawn-gated middle-Unlock assertion");
    require(naive.first_assertion->endpoint == step(2, 5) &&
                dpor.first_assertion->endpoint == step(2, 5),
            "middle-Unlock exploration reached the wrong assertion endpoint");
    require(naive.first_race.has_value() == dpor.first_race.has_value() &&
                naive.first_deadlock.has_value() == dpor.first_deadlock.has_value() &&
                naive.first_error.has_value() == dpor.first_error.has_value() &&
                (naive.cycles_detected > 0) == (dpor.cycles_detected > 0) &&
                (naive.fair_cycles > 0) == (dpor.fair_cycles > 0) &&
                (naive.strongly_unfair_cycles > 0) ==
                    (dpor.strongly_unfair_cycles > 0) &&
                (naive.unfair_cycles > 0) == (dpor.unfair_cycles > 0) &&
                dpor.schedules_explored <= naive.schedules_explored,
            "spawn-gated middle-Unlock naive/DPOR verdicts disagree");
    for (const model::CheckResult* result : {&naive, &dpor}) {
        const model::CheckResult report_replay =
            checker.replay(result->first_assertion->schedule);
        require(report_replay.first_assertion.has_value() &&
                    *report_replay.first_assertion == *result->first_assertion,
                "explored middle-Unlock assertion did not replay identically");
    }
}

void exact_reduction_preserves_each_free_mutex_winner_class() {
    // Exact count three is insufficient by itself: an unsound implementation
    // could prune one free-mutex winner and retain some other representative.
    // These asymmetric assertion endpoints make each winner class observable.
    const model::Program thread_zero_wins{{
        {
            try_lock("m", 0),
            branch_nonzero(0, "won"),
            set(7, 1),
            branch_nonzero(7, "done"),
            label("won"),
            assert_nonzero(7),
            label("done"),
        },
        {try_lock("m", 0)},
        {lock("m")},
    }};
    const model::ModelChecker thread_zero_checker(thread_zero_wins);
    const model::CheckResult thread_zero_naive =
        thread_zero_checker.explore_naive();
    const model::CheckResult thread_zero_dpor =
        thread_zero_checker.explore_dpor();
    require(thread_zero_naive.first_assertion.has_value() &&
                thread_zero_naive.first_assertion->endpoint == step(0, 5),
            "naive oracle did not expose the T0-wins TryLock assertion");
    require(thread_zero_dpor.first_assertion.has_value() &&
                thread_zero_dpor.first_assertion->endpoint == step(0, 5),
            "DPOR dropped the initially-free T0-wins TryLock class");
    const model::CheckResult thread_zero_replay =
        thread_zero_checker.replay(thread_zero_dpor.first_assertion->schedule);
    require(thread_zero_replay.first_assertion.has_value() &&
                *thread_zero_replay.first_assertion ==
                    *thread_zero_dpor.first_assertion,
            "T0-wins TryLock assertion did not replay identically");

    const model::Program thread_one_wins{{
        {try_lock("m", 0)},
        {
            try_lock("m", 0),
            branch_nonzero(0, "won"),
            set(7, 1),
            branch_nonzero(7, "done"),
            label("won"),
            assert_nonzero(7),
            label("done"),
        },
        {lock("m")},
    }};
    const model::ModelChecker thread_one_checker(thread_one_wins);
    const model::CheckResult thread_one_naive =
        thread_one_checker.explore_naive();
    const model::CheckResult thread_one_dpor =
        thread_one_checker.explore_dpor();
    require(thread_one_naive.first_assertion.has_value() &&
                thread_one_naive.first_assertion->endpoint == step(1, 5),
            "naive oracle did not expose the T1-wins TryLock assertion");
    require(thread_one_dpor.first_assertion.has_value() &&
                thread_one_dpor.first_assertion->endpoint == step(1, 5),
            "DPOR dropped the initially-free T1-wins TryLock class");
    const model::CheckResult thread_one_replay =
        thread_one_checker.replay(thread_one_dpor.first_assertion->schedule);
    require(thread_one_replay.first_assertion.has_value() &&
                *thread_one_replay.first_assertion ==
                    *thread_one_dpor.first_assertion,
            "T1-wins TryLock assertion did not replay identically");
}

void different_same_mutex_actions_do_not_use_failed_try_lock_refinement() {
    // After T2 acquires m, T0's failed TryLock and T1's non-owner Unlock are
    // exact enabled siblings but different actions. The state refinement must
    // not override their ordinary same-mutex dependence.
    const model::Program program{{
        {try_lock("m", 0)},
        {unlock("m")},
        {lock("m")},
    }};
    const model::ModelChecker checker(program);
    const model::CheckResult naive = checker.explore_naive();
    const model::CheckResult dpor = checker.explore_dpor();

    require(naive.schedules_explored == 4,
            "different-action guard changed its four naive terminal orders");
    require(dpor.schedules_explored == 4,
            "failed-TryLock refinement commuted a different same-mutex action");
    require(naive.first_error.has_value() && dpor.first_error.has_value(),
            "different-action guard lost its modeled non-owner Unlock error");
}

void third_party_owner_failed_try_locks_have_exact_reduction() {
    const model::Program program{{
        {try_lock("m", 0)},
        {try_lock("m", 0)},
        {lock("m")},
    }};
    const model::ModelChecker checker(program);
    const model::CheckResult naive = checker.explore_naive();
    const model::CheckResult dpor = checker.explore_dpor();

    // T2 first yields two failed-TryLock orders. If either trier goes first,
    // it acquires m and leaves only the other trier runnable before blocked
    // T2, contributing one order per winner. Only the T2-first pair collapses:
    // four naive schedules therefore have exactly three DPOR representatives.
    require(naive.schedules_explored == 4,
            "third-owner TryLock discriminator changed its four naive orders");
    require(dpor.schedules_explored == 3,
            "third-owner failed TryLocks retained four DPOR schedules instead of three");
}

void public_independence_is_conservative_for_one_mutex_and_commutes_distinct_mutexes() {
    const model::Action try_m = try_lock("m", 0);
    const model::Action other_try_m = try_lock("m", 1);
    const model::Action lock_m = lock("m");
    const model::Action unlock_m = unlock("m");
    const model::Action wait_m = wait("cv", "m");

    require(!model::independent(try_m, other_try_m) &&
                !model::independent(other_try_m, try_m),
            "same-mutex TryLock pair was publicly classified independent");
    require(!model::independent(try_m, lock_m) &&
                !model::independent(lock_m, try_m),
            "same-mutex TryLock/Lock pair was publicly classified independent");
    require(!model::independent(try_m, unlock_m) &&
                !model::independent(unlock_m, try_m),
            "same-mutex TryLock/Unlock pair was publicly classified independent");
    require(!model::independent(try_m, wait_m) &&
                !model::independent(wait_m, try_m),
            "same-mutex TryLock/Wait pair was publicly classified independent");

    const model::Action try_other = try_lock("other", 1);
    const model::Action lock_other = lock("other");
    const model::Action unlock_other = unlock("other");
    const model::Action wait_other = wait("other-cv", "other");
    require(model::independent(try_m, try_other) &&
                model::independent(try_other, try_m),
            "different-mutex TryLock pair did not commute");
    require(model::independent(try_m, lock_other) &&
                model::independent(lock_other, try_m),
            "different-mutex TryLock/Lock pair did not commute");
    require(model::independent(try_m, unlock_other) &&
                model::independent(unlock_other, try_m),
            "different-mutex TryLock/Unlock pair did not commute");
    require(model::independent(try_m, wait_other) &&
                model::independent(wait_other, try_m),
            "different-mutex TryLock/Wait pair did not commute");
}

void direct_api_try_lock_uses_the_existing_mutex_namespace_rules() {
    model::Action rlock;
    rlock.kind = model::ActionKind::RLock;
    rlock.rwlock = "shared";
    model::Action sem_post;
    sem_post.kind = model::ActionKind::SemPost;
    sem_post.semaphore = "shared";
    model::Action barrier_wait;
    barrier_wait.kind = model::ActionKind::BarrierWait;
    barrier_wait.barrier = "shared";
    barrier_wait.parties = 1;

    for (const model::Action& colliding : {rlock, sem_post, barrier_wait}) {
        bool rejected = false;
        try {
            (void)model::ModelChecker(model::Program{{{
                try_lock("shared", 0),
                colliding,
            }}});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected,
                "direct TryLock did not inherit a forbidden mutex namespace collision");
    }

    // Condition variables intentionally may reuse a mutex spelling. Keep that
    // established allowance while proving TryLock is registered as a mutex.
    model::Action signal;
    signal.kind = model::ActionKind::Signal;
    signal.condition = "shared";
    const model::Program allowed{{{
        try_lock("shared", 0),
        unlock("shared"),
        signal,
    }}};
    require_clean(model::ModelChecker(allowed).explore_naive(),
                  "direct TryLock changed the mutex/condition-name allowance");
}

} // namespace

int main() {
    free_try_lock_succeeds_and_owns_the_mutex();
    direct_api_try_lock_without_destination_defaults_to_r0();
    self_try_lock_fails_without_losing_the_original_ownership();
    held_try_lock_advances_without_blocking_or_deadlocking();
    failed_try_lock_finishes_cleanly_after_its_holder_has_finished();
    free_mutex_try_lock_pair_keeps_both_winner_orders();
    owner_that_is_one_trier_keeps_try_locks_dependent();
    mirrored_owner_that_is_one_trier_keeps_try_locks_dependent();
    unlock_between_failed_and_successful_try_locks_preserves_unique_assertion();
    exact_reduction_preserves_each_free_mutex_winner_class();
    different_same_mutex_actions_do_not_use_failed_try_lock_refinement();
    public_independence_is_conservative_for_one_mutex_and_commutes_distinct_mutexes();
    direct_api_try_lock_uses_the_existing_mutex_namespace_rules();
    third_party_owner_failed_try_locks_have_exact_reduction();
    return 0;
}
