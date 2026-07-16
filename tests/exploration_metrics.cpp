#include "model/checker.hpp"
#include "model/exploration_metrics.hpp"
#include "model/vector_clock.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace {

model::Action yield_action() {
    model::Action action;
    action.kind = model::ActionKind::Yield;
    return action;
}

model::Action set_one() {
    model::Action action;
    action.kind = model::ActionKind::Set;
    action.destination = 0;
    action.value = model::ValueOperand{
        model::ValueOperandKind::Immediate,
        1,
        0,
    };
    return action;
}

model::Action label(std::string name) {
    model::Action action;
    action.kind = model::ActionKind::Label;
    action.label = std::move(name);
    return action;
}

model::Action branch_nonzero(std::string target) {
    model::Action action;
    action.kind = model::ActionKind::BranchNonzero;
    action.source_register = 0;
    action.label = std::move(target);
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

model::Action try_lock(std::string mutex, model::RegisterId destination) {
    model::Action action;
    action.kind = model::ActionKind::TryLock;
    action.mutex = std::move(mutex);
    action.destination = destination;
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

model::Action write_one(std::string address) {
    model::Action action;
    action.kind = model::ActionKind::Write;
    action.address = std::move(address);
    action.value = model::ValueOperand{
        model::ValueOperandKind::Immediate,
        1,
        0,
    };
    return action;
}

model::Action spawn(model::ThreadId target) {
    model::Action action;
    action.kind = model::ActionKind::Spawn;
    action.target = target;
    return action;
}

model::Action join(model::ThreadId target) {
    model::Action action;
    action.kind = model::ActionKind::Join;
    action.target = target;
    return action;
}

model::Action barrier_wait(std::string barrier, std::uint32_t parties) {
    model::Action action;
    action.kind = model::ActionKind::BarrierWait;
    action.barrier = std::move(barrier);
    action.parties = parties;
    return action;
}

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

#define REQUIRE(expression) require((expression), #expression)

} // namespace

int main() {
    model::diagnostics::reset_exploration_metrics();

    const model::Program program{{{yield_action(), yield_action()}}};
    const model::CheckResult result = model::ModelChecker(program).explore_naive();
    const model::diagnostics::ExplorationMetrics metrics =
        model::diagnostics::exploration_metrics();

    REQUIRE(result.schedules_explored == 1);
    REQUIRE(metrics.executed_steps == 2);
    REQUIRE(metrics.branch_state_copies == 2);
    REQUIRE(metrics.state_history_copies == 0);
    REQUIRE(metrics.state_history_entries_copied == 0);
    REQUIRE(metrics.history_insertions == 0);
    REQUIRE(metrics.history_restores == 0);
    REQUIRE(metrics.fingerprint_builds == 0);
    REQUIRE(metrics.fingerprint_bytes == 0);
    REQUIRE(metrics.clock_ticks == 2);
    REQUIRE(metrics.clock_map_insertions == 1);
    REQUIRE(metrics.enabled_collections == 3);
    REQUIRE(metrics.enabled_thread_probes == 3);
    REQUIRE(metrics.enabled_steps_emitted == 2);
    REQUIRE(metrics.branch_state_schedule_steps_copied == 1);
    REQUIRE(metrics.branch_state_clock_components_copied == 1);
    REQUIRE(metrics.schedule_pushes == 2);
    REQUIRE(metrics.naive_dfs_entries == 3);

    model::diagnostics::reset_exploration_metrics();
    model::VectorClock release;
    release.tick(1);
    model::diagnostics::reset_exploration_metrics();
    model::VectorClock acquire;
    acquire.join(release);
    REQUIRE(acquire.happens_before_or_equal(release));
    const auto clock_metrics = model::diagnostics::exploration_metrics();
    REQUIRE(clock_metrics.clock_joins == 1);
    REQUIRE(clock_metrics.clock_join_components == 1);
    REQUIRE(clock_metrics.clock_comparisons == 1);
    REQUIRE(clock_metrics.clock_compare_components == 1);

    model::diagnostics::reset_exploration_metrics();
    const model::CheckResult dpor = model::ModelChecker(program).explore_dpor();
    const auto dpor_metrics = model::diagnostics::exploration_metrics();
    REQUIRE(dpor.schedules_explored == 1);
    REQUIRE(dpor_metrics.dpor_dfs_entries == 3);
    REQUIRE(dpor_metrics.dpor_node_snapshots == 3);
    REQUIRE(dpor_metrics.dpor_enabled_transition_maps == 3);
    REQUIRE(dpor_metrics.enabled_collections == 3);
    REQUIRE(dpor_metrics.enabled_thread_probes == 3);
    REQUIRE(dpor_metrics.enabled_steps_emitted == 2);
    REQUIRE(dpor_metrics.dpor_enabled_transition_entries == 2);
    REQUIRE(dpor_metrics.effective_actions_materialized == 2);
    REQUIRE(dpor_metrics.state_history_copies == 0);
    REQUIRE(dpor_metrics.history_insertions == 0);
    REQUIRE(dpor_metrics.history_restores == 0);

    model::diagnostics::reset_exploration_metrics();
    const model::Program branching{{
        {yield_action(), yield_action()},
        {yield_action(), yield_action()},
    }};
    const model::CheckResult branching_naive =
        model::ModelChecker(branching).explore_naive();
    const auto branching_metrics = model::diagnostics::exploration_metrics();
    REQUIRE(branching_naive.schedules_explored == 6);
    REQUIRE(branching_metrics.state_history_copies == 0);
    REQUIRE(branching_metrics.history_insertions == 0);
    REQUIRE(branching_metrics.history_restores == 0);

    model::diagnostics::reset_exploration_metrics();
    const model::Schedule complete{{0, 0, std::nullopt}, {0, 1, std::nullopt}};
    const model::CheckResult replayed = model::ModelChecker(program).replay(complete);
    const auto replay_metrics = model::diagnostics::exploration_metrics();
    REQUIRE(replayed.schedules_explored == 1);
    REQUIRE(replay_metrics.replay_calls == 1);
    REQUIRE(replay_metrics.fingerprint_builds == 0);

    model::diagnostics::reset_exploration_metrics();
    const model::Program forward_branch{{{
        set_one(),
        branch_nonzero("done"),
        yield_action(),
        label("skip"),
        label("done"),
        yield_action(),
    }}};
    const model::CheckResult forward =
        model::ModelChecker(forward_branch).explore_naive();
    const auto forward_metrics = model::diagnostics::exploration_metrics();
    REQUIRE(forward.schedules_explored == 1);
    REQUIRE(!forward.first_error.has_value());
    REQUIRE(!forward.first_nontermination.has_value());
    REQUIRE(forward_metrics.fingerprint_builds == 0);

    model::diagnostics::reset_exploration_metrics();
    const model::Program acyclic_try_lock{{{
        try_lock("m", 0),
        unlock("m"),
    }}};
    const model::CheckResult acyclic_try_result =
        model::ModelChecker(acyclic_try_lock).explore_naive();
    const auto acyclic_try_metrics =
        model::diagnostics::exploration_metrics();
    REQUIRE(acyclic_try_result.schedules_explored == 1);
    REQUIRE(!acyclic_try_result.first_error.has_value());
    REQUIRE(!acyclic_try_result.first_nontermination.has_value());
    REQUIRE(acyclic_try_metrics.fingerprint_builds == 0);
    REQUIRE(acyclic_try_metrics.history_insertions == 0);
    REQUIRE(acyclic_try_metrics.history_restores == 0);

    model::diagnostics::reset_exploration_metrics();
    const model::Program missing_label{{{
        set_one(),
        branch_nonzero("missing"),
    }}};
    const model::CheckResult missing =
        model::ModelChecker(missing_label).explore_naive();
    const auto missing_metrics = model::diagnostics::exploration_metrics();
    REQUIRE(missing.schedules_explored == 1);
    REQUIRE(missing.first_error.has_value());
    REQUIRE(missing_metrics.fingerprint_builds == 0);

    model::diagnostics::reset_exploration_metrics();
    const model::Program condition_handoff{{
        {lock("m"), wait("cv", "m"), unlock("m")},
        {lock("m"), signal("cv"), unlock("m")},
    }};
    const model::CheckResult handoff =
        model::ModelChecker(condition_handoff).explore_naive();
    const auto handoff_metrics = model::diagnostics::exploration_metrics();
    REQUIRE(handoff.schedules_explored > 0);
    REQUIRE(!handoff.first_nontermination.has_value());
    REQUIRE(handoff_metrics.fingerprint_builds == 0);

    for (const model::MemoryModel memory_model :
         {model::MemoryModel::TSO, model::MemoryModel::PSO}) {
        model::diagnostics::reset_exploration_metrics();
        const model::Program buffered{{{write_one("x"), write_one("y")}}};
        const model::CheckResult drained =
            model::ModelChecker(buffered,
                                model::ModelChecker::kDefaultStepBound,
                                memory_model)
                .explore_naive();
        const auto buffered_metrics = model::diagnostics::exploration_metrics();
        REQUIRE(drained.schedules_explored > 0);
        REQUIRE(!drained.first_nontermination.has_value());
        REQUIRE(buffered_metrics.fingerprint_builds == 0);
    }

    model::diagnostics::reset_exploration_metrics();
    const model::Program spawned{{
        {spawn(1), join(1)},
        {yield_action()},
    }};
    const model::CheckResult joined = model::ModelChecker(spawned).explore_naive();
    const auto spawned_metrics = model::diagnostics::exploration_metrics();
    REQUIRE(joined.schedules_explored > 0);
    REQUIRE(!joined.first_error.has_value());
    REQUIRE(!joined.first_nontermination.has_value());
    REQUIRE(spawned_metrics.fingerprint_builds == 0);

    model::diagnostics::reset_exploration_metrics();
    const model::Program completed_barrier{{
        {barrier_wait("phase", 2)},
        {barrier_wait("phase", 2)},
    }};
    const model::CheckResult completed =
        model::ModelChecker(completed_barrier).explore_naive();
    const auto completed_barrier_metrics =
        model::diagnostics::exploration_metrics();
    REQUIRE(completed.schedules_explored > 0);
    REQUIRE(!completed.first_deadlock.has_value());
    REQUIRE(!completed.first_nontermination.has_value());
    REQUIRE(completed_barrier_metrics.fingerprint_builds == 0);

    model::diagnostics::reset_exploration_metrics();
    const model::Program incomplete_barrier{{
        {barrier_wait("phase", 3)},
        {barrier_wait("phase", 3)},
    }};
    const model::CheckResult incomplete =
        model::ModelChecker(incomplete_barrier).explore_naive();
    const auto incomplete_barrier_metrics =
        model::diagnostics::exploration_metrics();
    REQUIRE(incomplete.first_deadlock.has_value());
    REQUIRE(!incomplete.first_nontermination.has_value());
    REQUIRE(incomplete_barrier_metrics.fingerprint_builds == 0);

    model::diagnostics::reset_exploration_metrics();
    const model::Program blocked_barrier_loop{{
        {set_one(), label("loop"), barrier_wait("phase", 3),
         branch_nonzero("loop")},
        {set_one(), label("loop"), barrier_wait("phase", 3),
         branch_nonzero("loop")},
    }};
    const model::CheckResult blocked_loop =
        model::ModelChecker(blocked_barrier_loop, 10).explore_naive();
    const auto blocked_barrier_metrics =
        model::diagnostics::exploration_metrics();
    REQUIRE(blocked_loop.first_deadlock.has_value());
    REQUIRE(blocked_loop.cycles_detected == 0);
    REQUIRE(blocked_barrier_metrics.fingerprint_builds > 0);

    model::diagnostics::reset_exploration_metrics();
    const model::Program cyclic_barrier{{
        {set_one(), label("loop"), barrier_wait("phase", 2),
         branch_nonzero("loop")},
        {set_one(), label("loop"), barrier_wait("phase", 2),
         branch_nonzero("loop")},
    }};
    const model::CheckResult barrier_cycle =
        model::ModelChecker(cyclic_barrier, 10).explore_naive();
    const auto cyclic_barrier_metrics =
        model::diagnostics::exploration_metrics();
    REQUIRE(barrier_cycle.cycles_detected > 0);
    REQUIRE(barrier_cycle.first_nontermination.has_value());
    REQUIRE(cyclic_barrier_metrics.fingerprint_builds > 0);
    REQUIRE(cyclic_barrier_metrics.history_insertions ==
            cyclic_barrier_metrics.history_restores);

    model::diagnostics::reset_exploration_metrics();
    const model::Program failed_try_retry{{
        {lock("m"), spawn(1)},
        {
            set_one(),
            label("retry"),
            try_lock("m", 1),
            branch_nonzero("retry"),
        },
    }};
    const model::CheckResult failed_try_cycle =
        model::ModelChecker(failed_try_retry, 10).explore_naive();
    const auto failed_try_metrics =
        model::diagnostics::exploration_metrics();
    REQUIRE(failed_try_cycle.cycles_detected > 0);
    REQUIRE(failed_try_cycle.first_nontermination.has_value());
    REQUIRE(failed_try_metrics.fingerprint_builds > 0);
    REQUIRE(failed_try_metrics.history_insertions > 0);
    REQUIRE(failed_try_metrics.history_insertions ==
            failed_try_metrics.history_restores);

    model::diagnostics::reset_exploration_metrics();
    const model::Program cyclic{{{
        set_one(),
        label("loop"),
        yield_action(),
        branch_nonzero("loop"),
    }}};
    const model::CheckResult cycle = model::ModelChecker(cyclic, 10).explore_naive();
    const auto cycle_metrics = model::diagnostics::exploration_metrics();
    REQUIRE(cycle.cycles_detected > 0);
    REQUIRE(cycle.first_nontermination.has_value());
    REQUIRE(cycle_metrics.fingerprint_builds > 0);
    REQUIRE(cycle_metrics.history_insertions > 0);
    REQUIRE(cycle_metrics.history_insertions == cycle_metrics.history_restores);

    model::diagnostics::reset_exploration_metrics();
    REQUIRE(model::diagnostics::exploration_metrics() ==
            model::diagnostics::ExplorationMetrics{});
    return 0;
}
