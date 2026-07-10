#include "model/checker.hpp"
#include "report.hpp"

#include <cassert>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

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

model::Action assert_nonzero(model::RegisterId reg) {
    model::Action action;
    action.kind = model::ActionKind::Assert;
    action.source_register = reg;
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

model::Action atomic_rmw(std::string address, model::Value value, model::RegisterId destination) {
    model::Action action;
    action.kind = model::ActionKind::AtomicRmw;
    action.address = std::move(address);
    action.value = imm(value);
    action.destination = destination;
    return action;
}

model::Action write(std::string address, model::Value value) {
    model::Action action;
    action.kind = model::ActionKind::Write;
    action.address = std::move(address);
    action.value = imm(value);
    return action;
}

model::Action fence() {
    model::Action action;
    action.kind = model::ActionKind::Fence;
    return action;
}

void require_same_nontermination(const model::CheckResult& expected,
                                 const model::CheckResult& replayed) {
    assert(expected.first_nontermination.has_value());
    assert(replayed.first_nontermination.has_value());
    assert(*replayed.first_nontermination == *expected.first_nontermination);
}

void pure_spin_has_a_one_step_cycle_and_replays_identically() {
    const model::Program program{{
        {set(1, 1), label("spin"), bnz(1, "spin")},
    }};
    const model::ModelChecker checker(program, 20);

    const model::CheckResult naive = checker.explore_naive();
    const model::CheckResult dpor = checker.explore_dpor();

    assert(naive.first_nontermination.has_value());
    assert(dpor.first_nontermination.has_value());
    assert(naive.cycles_detected == 1);
    assert(dpor.cycles_detected == 1);
    assert(naive.bound_exceeded_executions == 0);
    assert(dpor.bound_exceeded_executions == 0);
    assert(dpor.first_nontermination->stem == (model::Schedule{{0, 0}}));
    assert(dpor.first_nontermination->cycle == (model::Schedule{{0, 2}}));
    assert(dpor.first_nontermination->schedule ==
           (model::Schedule{{0, 0}, {0, 2}}));

    const model::CheckResult replayed = checker.replay(dpor.first_nontermination->schedule);
    require_same_nontermination(dpor, replayed);
    assert(replayed.cycles_detected == 1);

    // Lasso schedules are intentionally shipped unminimized: the public
    // minimizer must not claim a deletion-minimal cycle it cannot prove.
    assert(checker.minimize_schedule(dpor.first_nontermination->schedule) ==
           dpor.first_nontermination->schedule);

    model::Schedule continued = dpor.first_nontermination->schedule;
    continued.push_back({0, 2});
    try {
        (void)checker.replay(continued);
        assert(false && "replay should reject a schedule that continues after a lasso closes");
    } catch (const std::invalid_argument& error) {
        assert(std::string(error.what()).find("continues after a nontermination cycle") !=
               std::string::npos);
    }
}

void unfair_spin_cycle_exists_even_though_a_peer_can_complete_it() {
    const model::Program program{{
        {
            set(1, 1),
            label("spin"),
            atomic_load("f", 0),
            bnz(0, "done"),
            bnz(1, "spin"),
            label("done"),
            assert_nonzero(1),
        },
        {atomic_store("f", 1)},
    }};
    const model::ModelChecker checker(program, 20);

    const model::CheckResult naive = checker.explore_naive();
    const model::CheckResult dpor = checker.explore_dpor();
    assert(naive.first_nontermination.has_value());
    assert(dpor.first_nontermination.has_value());
    assert(naive.bound_exceeded_executions == 0);
    assert(dpor.bound_exceeded_executions == 0);

    const model::Schedule completing{{1, 0}, {0, 0}, {0, 2}, {0, 3}, {0, 6}};
    const model::CheckResult completed = checker.replay(completing);
    assert(completed.schedules_explored == 1);
    assert(!completed.first_race.has_value());
    assert(!completed.first_deadlock.has_value());
    assert(!completed.first_error.has_value());
    assert(!completed.first_assertion.has_value());
    assert(!completed.first_nontermination.has_value());
}

void growing_fetch_add_loop_uses_the_bound_backstop() {
    const model::Program program{{
        {
            set(1, 1),
            label("grow"),
            atomic_rmw("counter", 1, 0),
            bnz(1, "grow"),
        },
    }};
    const model::ModelChecker checker(program, 8);

    const model::CheckResult naive = checker.explore_naive();
    const model::CheckResult dpor = checker.explore_dpor();
    assert(!naive.first_nontermination.has_value());
    assert(!dpor.first_nontermination.has_value());
    assert(naive.cycles_detected == 0);
    assert(dpor.cycles_detected == 0);
    assert(naive.bound_exceeded_executions == 1);
    assert(dpor.bound_exceeded_executions == 1);
    assert(cli::verdict_of(dpor) == "clean up to bound");
}

void tso_cycle_closes_only_after_the_store_buffer_repeats() {
    const model::Program program{{
        {
            set(1, 1),
            label("spin"),
            write("x", 1),
            fence(),
            bnz(1, "spin"),
        },
    }};
    const model::ModelChecker checker(program, 20, model::MemoryModel::TSO);

    const model::CheckResult naive = checker.explore_naive();
    const model::CheckResult dpor = checker.explore_dpor();
    assert(naive.first_nontermination.has_value());
    assert(dpor.first_nontermination.has_value());
    // The first write/flush changes shared x from zero to one and reaches the
    // earliest recurring state at the fence with an empty buffer. Omitting the
    // buffer would falsely close the cycle immediately after the second write;
    // the exact fingerprint requires the final flush to restore empty-buffer
    // state before the cycle can close.
    assert(dpor.first_nontermination->stem ==
           (model::Schedule{{0, 0},
                            {0, 2},
                            {0, model::kFlushActionIndex}}));
    assert(dpor.first_nontermination->cycle ==
           (model::Schedule{{0, 3}, {0, 4}, {0, 2}, {0, model::kFlushActionIndex}}));
    assert(dpor.first_nontermination->cycle.back().action_index == model::kFlushActionIndex);
    require_same_nontermination(
        dpor, checker.replay(dpor.first_nontermination->schedule));
}

void cli_renders_nontermination_stem_cycle_and_replay_schedule() {
    const model::Program program{{
        {set(1, 1), label("spin"), bnz(1, "spin")},
    }};
    const model::CheckResult result = model::ModelChecker(program, 20).explore_dpor();
    assert(cli::verdict_of(result) == "nontermination");
    assert(cli::has_bug(result));

    std::ostringstream output;
    cli::print_report(output, program, result, model::MemoryModel::SC, 20);
    const std::string text = output.str();
    assert(text.find("verdict: nontermination\n") == 0);
    assert(text.find("cycles_detected: 1\n") != std::string::npos);
    assert(text.find("nontermination:\n  stem:\n    0 0\n  cycle:\n    0 2\n") !=
           std::string::npos);
    assert(text.find("schedule:\n  0 0\n  0 2\n") != std::string::npos);
}

void cli_lists_nontermination_when_a_higher_priority_race_also_exists() {
    const model::Program program{{
        {write("x", 1), set(1, 1), label("spin"), bnz(1, "spin")},
        {write("x", 2)},
    }};
    const model::CheckResult result = model::ModelChecker(program, 20).explore_dpor();
    assert(result.first_race.has_value());
    assert(result.first_nontermination.has_value());
    assert(cli::verdict_of(result) == "race");

    std::ostringstream output;
    cli::print_report(output, program, result, model::MemoryModel::SC, 20);
    assert(output.str().find("also_found: nontermination\n") != std::string::npos);
}

} // namespace

int main() {
    pure_spin_has_a_one_step_cycle_and_replays_identically();
    unfair_spin_cycle_exists_even_though_a_peer_can_complete_it();
    growing_fetch_add_loop_uses_the_bound_backstop();
    tso_cycle_closes_only_after_the_store_buffer_repeats();
    cli_renders_nontermination_stem_cycle_and_replay_schedule();
    cli_lists_nontermination_when_a_higher_priority_race_also_exists();
    return 0;
}
