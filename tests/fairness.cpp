#ifdef NDEBUG
#undef NDEBUG
#endif

#include "model/checker.hpp"
#include "report.hpp"

#include <algorithm>
#include <cassert>
#include <sstream>
#include <string>
#include <type_traits>

namespace {

static_assert(static_cast<std::underlying_type_t<model::Fairness>>(
                  model::Fairness::UnfairScheduleWitness) == 0);
static_assert(static_cast<std::underlying_type_t<model::Fairness>>(
                  model::Fairness::FairDivergence) == 1);
static_assert(static_cast<std::underlying_type_t<model::Fairness>>(
                  model::Fairness::StronglyUnfairScheduleWitness) == 2);

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

model::Action atomic_load(std::string address, model::RegisterId destination) {
    model::Action action;
    action.kind = model::ActionKind::AtomicLoad;
    action.address = std::move(address);
    action.destination = destination;
    return action;
}

model::Action atomic_store(std::string address, model::Value value) {
    model::Action action;
    action.kind = model::ActionKind::AtomicStore;
    action.address = std::move(address);
    action.value = imm(value);
    return action;
}

model::Action write(std::string address, model::Value value) {
    model::Action action;
    action.kind = model::ActionKind::Write;
    action.address = std::move(address);
    action.value = imm(value);
    return action;
}

model::Action lock(std::string mutex) {
    model::Action action;
    action.kind = model::ActionKind::Lock;
    action.mutex = std::move(mutex);
    return action;
}

model::Action try_lock(std::string mutex, model::RegisterId destination) {
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

model::Action wait(std::string condition, std::string mutex) {
    model::Action action;
    action.kind = model::ActionKind::Wait;
    action.condition = std::move(condition);
    action.mutex = std::move(mutex);
    return action;
}

model::Action timed_wait(std::string condition,
                         std::string mutex,
                         model::RegisterId destination) {
    model::Action action;
    action.kind = model::ActionKind::TimedWait;
    action.condition = std::move(condition);
    action.mutex = std::move(mutex);
    action.destination = destination;
    return action;
}

model::Action signal(std::string condition) {
    model::Action action;
    action.kind = model::ActionKind::Signal;
    action.condition = std::move(condition);
    return action;
}

model::Action yield() {
    model::Action action;
    action.kind = model::ActionKind::Yield;
    return action;
}

void require_replay_identity(const model::ModelChecker& checker,
                             const model::CheckResult& explored) {
    assert(explored.first_nontermination.has_value());
    const model::CheckResult replayed =
        checker.replay(explored.first_nontermination->schedule);
    assert(replayed.first_nontermination == explored.first_nontermination);
    assert(replayed.fair_cycles + replayed.strongly_unfair_cycles +
               replayed.unfair_cycles ==
           replayed.cycles_detected);
    assert(replayed.fair_cycles ==
           (explored.first_nontermination->fairness == model::Fairness::FairDivergence ? 1U : 0U));
    assert(replayed.strongly_unfair_cycles ==
           (explored.first_nontermination->fairness ==
                    model::Fairness::StronglyUnfairScheduleWitness
                ? 1U
                : 0U));
    assert(replayed.unfair_cycles ==
           (explored.first_nontermination->fairness ==
                    model::Fairness::UnfairScheduleWitness
                ? 1U
                : 0U));
}

void finished_peer_does_not_make_a_pure_spin_unfair() {
    const model::Program program{{
        {set(1, 1), label("spin"), atomic_load("never", 0), bnz(1, "spin")},
        {},
    }};
    const model::ModelChecker checker(program, 20);

    const model::CheckResult naive = checker.explore_naive();
    const model::CheckResult dpor = checker.explore_dpor();
    assert(naive.first_nontermination->fairness == model::Fairness::FairDivergence);
    assert(dpor.first_nontermination->fairness == model::Fairness::FairDivergence);
    assert(naive.fair_cycles > 0 && naive.unfair_cycles == 0);
    assert(dpor.fair_cycles > 0 && dpor.unfair_cycles == 0);
    assert(naive.strongly_unfair_cycles == 0);
    assert(dpor.strongly_unfair_cycles == 0);
    require_replay_identity(checker, dpor);

    std::ostringstream output;
    cli::print_report(output, program, dpor, model::MemoryModel::SC, 20);
    assert(output.str().find("  fairness: fair divergence\n") != std::string::npos);
}

void continuously_enabled_flag_setter_makes_the_spin_witness_unfair() {
    const model::Program program{{
        {set(1, 1), label("spin"), atomic_load("flag", 0), bnz(1, "spin")},
        {atomic_store("flag", 1)},
    }};
    const model::ModelChecker checker(program, 20);

    const model::CheckResult naive = checker.explore_naive();
    const model::CheckResult dpor = checker.explore_dpor();
    assert(naive.first_nontermination->fairness ==
           model::Fairness::UnfairScheduleWitness);
    assert(dpor.first_nontermination->fairness ==
           model::Fairness::UnfairScheduleWitness);
    assert(naive.unfair_cycles > 0);
    assert(dpor.unfair_cycles > 0);
    assert(naive.strongly_unfair_cycles == 0);
    assert(dpor.strongly_unfair_cycles == 0);
    require_replay_identity(checker, dpor);

    std::ostringstream output;
    cli::print_report(output, program, dpor, model::MemoryModel::SC, 20);
    assert(output.str().find("  fairness: unfair-schedule witness\n") !=
           std::string::npos);
}

void continuously_enabled_signaler_makes_timeout_spin_unfair() {
    // Thread 0 can perpetually choose its timeout, reacquire m, and re-park.
    // Thread 1's exact Signal endpoint remains enabled at every state in that
    // lasso, so omitting the nonparticipant is a weak-fairness violation under
    // ADR 0026 rather than fair divergence.
    const model::Program program{{
        {
            set(1, 1),
            lock("m"),
            label("spin"),
            timed_wait("cv", "m", 0),
            bnz(1, "spin"),
        },
        {signal("cv")},
    }};
    const model::ModelChecker checker(program, 20);
    const model::Schedule witness{
        {0, 0, std::nullopt},
        {0, 1, std::nullopt},
        {0, 3, std::nullopt},
        {0, 3, std::nullopt},
        {0, 3, std::nullopt},
        {0, 4, std::nullopt},
    };

    const model::CheckResult replayed = checker.replay(witness);
    assert(!replayed.first_deadlock.has_value());
    assert(replayed.first_nontermination.has_value());
    assert(replayed.first_nontermination->schedule == witness);
    assert(replayed.first_nontermination->cycle ==
           (model::Schedule{
               {0, 3, std::nullopt},
               {0, 3, std::nullopt},
               {0, 3, std::nullopt},
               {0, 4, std::nullopt},
           }));
    assert(replayed.first_nontermination->fairness ==
           model::Fairness::UnfairScheduleWitness);
    assert(replayed.unfair_cycles == 1);
    assert(replayed.strongly_unfair_cycles == 0);
    assert(replayed.fair_cycles == 0);
    require_replay_identity(checker, replayed);

    const model::CheckResult naive = checker.explore_naive();
    const model::CheckResult dpor = checker.explore_dpor();
    assert(naive.first_nontermination.has_value());
    assert(dpor.first_nontermination.has_value());
    assert(naive.first_nontermination->fairness ==
           model::Fairness::UnfairScheduleWitness);
    assert(dpor.first_nontermination->fairness ==
           model::Fairness::UnfairScheduleWitness);
    assert(naive.unfair_cycles > 0);
    assert(dpor.unfair_cycles > 0);
}

void intermittently_enabled_mutex_waiter_makes_the_witness_strongly_unfair() {
    // Thread 1's exact Lock(m) endpoint is enabled after thread 0 unlocks,
    // then disabled again after thread 0 retakes m. It is enabled in every
    // cycle iteration but not continuously through the cycle.
    const model::Program program{{
        {
            set(7, 1),
            lock("m"),
            label("retry"),
            unlock("m"),
            lock("m"),
            bnz(7, "retry"),
        },
        {lock("m")},
    }};
    const model::ModelChecker checker(program, 20);
    const model::Schedule witness{
        {0, 0, std::nullopt},
        {0, 1, std::nullopt},
        {0, 3, std::nullopt},
        {0, 4, std::nullopt},
        {0, 5, std::nullopt},
    };

    const model::CheckResult replayed = checker.replay(witness);
    assert(replayed.first_nontermination.has_value());
    assert(replayed.first_nontermination->fairness ==
           model::Fairness::StronglyUnfairScheduleWitness);
    assert(replayed.first_nontermination->cycle ==
           (model::Schedule{{0, 3, std::nullopt},
                            {0, 4, std::nullopt},
                            {0, 5, std::nullopt}}));
    assert(replayed.strongly_unfair_cycles == 1);
    assert(replayed.unfair_cycles == 0);
    assert(replayed.fair_cycles == 0);
    require_replay_identity(checker, replayed);

    std::ostringstream output;
    cli::print_report(output, program, replayed, model::MemoryModel::SC, 20);
    assert(output.str().find(
               "  fairness: strongly-unfair-schedule witness\n") !=
           std::string::npos);

    const model::CheckResult naive = checker.explore_naive();
    const model::CheckResult dpor = checker.explore_dpor();
    assert(naive.strongly_unfair_cycles > 0);
    assert(dpor.strongly_unfair_cycles > 0);
}

void mutual_backoff_cycle_with_both_threads_participating_is_fair_divergence() {
    // The condition-variable baton prevents either thread from cycling alone.
    // In every round both flags become one, both peers observe the other's one,
    // both back off to zero, and both retry.
    const model::Program program{{
        {
            set(7, 1),
            lock("m"),
            wait("phase0", "m"),
            label("retry0"),
            atomic_store("flag0", 1),
            signal("phase1"),
            wait("phase0", "m"),
            atomic_load("flag1", 0),
            signal("phase1"),
            wait("phase0", "m"),
            atomic_store("flag0", 0),
            signal("phase1"),
            wait("phase0", "m"),
            bnz(7, "retry0"),
        },
        {
            set(7, 1),
            lock("m"),
            label("retry1"),
            atomic_store("flag1", 1),
            signal("phase0"),
            wait("phase1", "m"),
            atomic_load("flag0", 0),
            signal("phase0"),
            wait("phase1", "m"),
            atomic_store("flag1", 0),
            signal("phase0"),
            wait("phase1", "m"),
            bnz(7, "retry1"),
        },
    }};
    const model::ModelChecker checker(program, 40);
    const model::CheckResult dpor = checker.explore_dpor();

    assert(dpor.first_nontermination.has_value());
    assert(dpor.first_nontermination->fairness == model::Fairness::FairDivergence);
    const auto& cycle = dpor.first_nontermination->cycle;
    assert(std::any_of(cycle.begin(), cycle.end(),
                       [](const model::ScheduleStep& step) { return step.thread == 0; }));
    assert(std::any_of(cycle.begin(), cycle.end(),
                       [](const model::ScheduleStep& step) { return step.thread == 1; }));
    require_replay_identity(checker, dpor);
}

void mutex_blocked_nonparticipant_does_not_make_the_cycle_unfair() {
    const model::Program program{{
        {lock("held"), set(1, 1), label("spin"), bnz(1, "spin")},
        {lock("held")},
    }};
    const model::ModelChecker checker(program, 20);
    const model::CheckResult dpor = checker.explore_dpor();

    assert(dpor.first_nontermination.has_value());
    assert(dpor.first_nontermination->fairness == model::Fairness::FairDivergence);
    assert(dpor.fair_cycles > 0);
    assert(dpor.strongly_unfair_cycles == 0);
    assert(dpor.unfair_cycles == 0);
    require_replay_identity(checker, dpor);
}

void a_pending_flush_is_enabled_for_weak_fairness_under_tso_and_pso() {
    const model::Program program{{
        {write("buffered", 1)},
        {set(1, 1), label("spin"), bnz(1, "spin")},
    }};
    const model::Schedule witness{
        {0, 0, std::nullopt},
        {1, 0, std::nullopt},
        {1, 2, std::nullopt},
    };

    for (const model::MemoryModel memory_model :
         {model::MemoryModel::TSO, model::MemoryModel::PSO}) {
        const model::ModelChecker checker(program, 20, memory_model);
        const model::CheckResult replayed = checker.replay(witness);
        assert(replayed.first_nontermination.has_value());
        assert(replayed.first_nontermination->fairness ==
               model::Fairness::UnfairScheduleWitness);
        assert(replayed.unfair_cycles == 1);
        assert(replayed.fair_cycles == 0);
    }
}

void first_report_remains_first_found_while_counters_track_both_classes() {
    const model::Program program{{
        {set(1, 1), label("spin"), bnz(1, "spin")},
        {yield()},
    }};
    const model::ModelChecker checker(program, 20);

    const model::CheckResult naive = checker.explore_naive();
    const model::CheckResult dpor = checker.explore_dpor();
    assert(naive.first_nontermination->fairness ==
           model::Fairness::UnfairScheduleWitness);
    assert(dpor.first_nontermination->fairness ==
           model::Fairness::UnfairScheduleWitness);
    assert(naive.fair_cycles > 0 && naive.unfair_cycles > 0);
    assert(dpor.fair_cycles > 0 && dpor.unfair_cycles > 0);
}

void failed_try_lock_spinner_is_unfair_while_the_holder_can_unlock() {
    const model::Program program{{
        {lock("m"), unlock("m")},
        {
            set(7, 1),
            label("retry"),
            try_lock("m", 0),
            bnz(7, "retry"),
        },
    }};
    const model::ModelChecker checker(program, 20);
    const model::Schedule witness{
        {0, 0, std::nullopt},
        {1, 0, std::nullopt},
        {1, 2, std::nullopt},
        {1, 3, std::nullopt},
    };

    const model::CheckResult witnessed = checker.replay(witness);
    assert(witnessed.first_nontermination.has_value());
    assert(witnessed.first_nontermination->fairness ==
           model::Fairness::UnfairScheduleWitness);
    assert(witnessed.first_nontermination->schedule == witness);
    assert(witnessed.first_nontermination->cycle ==
           (model::Schedule{{1, 2, std::nullopt}, {1, 3, std::nullopt}}));
    assert(std::all_of(witnessed.first_nontermination->cycle.begin(),
                       witnessed.first_nontermination->cycle.end(),
                       [](const model::ScheduleStep& step) {
                           return step.thread == 1;
                       }));
    require_replay_identity(checker, witnessed);

    const model::CheckResult naive = checker.explore_naive();
    const model::CheckResult dpor = checker.explore_dpor();
    assert(naive.unfair_cycles > 0);
    assert(dpor.unfair_cycles > 0);
}

} // namespace

int main() {
    finished_peer_does_not_make_a_pure_spin_unfair();
    continuously_enabled_flag_setter_makes_the_spin_witness_unfair();
    continuously_enabled_signaler_makes_timeout_spin_unfair();
    intermittently_enabled_mutex_waiter_makes_the_witness_strongly_unfair();
    mutual_backoff_cycle_with_both_threads_participating_is_fair_divergence();
    mutex_blocked_nonparticipant_does_not_make_the_cycle_unfair();
    a_pending_flush_is_enabled_for_weak_fairness_under_tso_and_pso();
    first_report_remains_first_found_while_counters_track_both_classes();
    failed_try_lock_spinner_is_unfair_while_the_holder_can_unlock();
    return 0;
}
