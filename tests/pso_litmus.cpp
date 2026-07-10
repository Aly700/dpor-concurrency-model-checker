#include "model/checker.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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

model::Action assertion(model::RegisterId reg) {
    model::Action action;
    action.kind = model::ActionKind::Assert;
    action.source_register = reg;
    return action;
}

model::Action read(std::string address, model::RegisterId destination) {
    model::Action action;
    action.kind = model::ActionKind::Read;
    action.address = std::move(address);
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

model::Action cas(std::string address,
                  model::Value expected,
                  model::Value desired,
                  model::RegisterId destination) {
    model::Action action;
    action.kind = model::ActionKind::CompareExchange;
    action.address = std::move(address);
    action.expected = imm(expected);
    action.value = imm(desired);
    action.destination = destination;
    return action;
}

model::Action fence() {
    model::Action action;
    action.kind = model::ActionKind::Fence;
    return action;
}

model::Action join(model::ThreadId target) {
    model::Action action;
    action.kind = model::ActionKind::Join;
    action.target = target;
    return action;
}

model::ScheduleStep pso_flush(model::ThreadId thread, std::uint32_t address_id) {
    model::ScheduleStep step;
    step.thread = thread;
    step.action_index = model::kFlushActionIndex;
    step.flush_address = address_id;
    return step;
}

struct BugShape {
    bool race{false};
    bool deadlock{false};
    bool error{false};
    bool assertion{false};
    bool nontermination{false};
};

void require_shape(const char* label, const model::CheckResult& result, BugShape expected) {
    const BugShape actual{
        result.first_race.has_value(),
        result.first_deadlock.has_value(),
        result.first_error.has_value(),
        result.first_assertion.has_value(),
        result.first_nontermination.has_value(),
    };
    if (actual.race != expected.race ||
        actual.deadlock != expected.deadlock ||
        actual.error != expected.error ||
        actual.assertion != expected.assertion ||
        actual.nontermination != expected.nontermination ||
        result.exploration_capped) {
        std::cerr << label
                  << " race=" << actual.race
                  << " deadlock=" << actual.deadlock
                  << " error=" << actual.error
                  << " assertion=" << actual.assertion
                  << " nontermination=" << actual.nontermination
                  << " capped=" << result.exploration_capped
                  << " schedules=" << result.schedules_explored << '\n';
        throw std::runtime_error("unexpected PSO litmus result shape");
    }
}

std::vector<model::CheckResult> explore_all_models(const model::Program& program,
                                                    std::size_t step_bound,
                                                    std::size_t max_schedules) {
    std::vector<model::CheckResult> results;
    for (const model::MemoryModel memory_model : {
             model::MemoryModel::SC,
             model::MemoryModel::TSO,
             model::MemoryModel::PSO,
         }) {
        results.push_back(
            model::ModelChecker(program, step_bound, memory_model).explore_dpor(max_schedules));
    }
    return results;
}

model::Program message_passing_program(bool fenced) {
    std::vector<model::Action> producer{write("data", 1)};
    if (fenced) {
        producer.push_back(fence());
    }
    producer.push_back(write("flag", 1));

    return model::Program{{
        std::move(producer),
        {
            read("flag", 0),
            bnz(0, "saw_flag"),
            set(7, 1),
            bnz(7, "done"),
            label("saw_flag"),
            read("data", 1),
            assertion(1),
            label("done"),
        },
    }};
}

model::Program store_buffering_program() {
    return model::Program{{
        {
            write("x", 1),
            read("y", 0),
            bnz(0, "done"),
            atomic_store("t0_zero", 1),
            label("done"),
        },
        {
            write("y", 1),
            read("x", 0),
            bnz(0, "done"),
            atomic_store("t1_zero", 1),
            label("done"),
        },
        {
            join(0),
            join(1),
            atomic_load("t0_zero", 0),
            bnz(0, "check_other"),
            set(6, 1),
            bnz(6, "done"),
            label("check_other"),
            atomic_load("t1_zero", 1),
            bnz(1, "fail"),
            set(6, 1),
            bnz(6, "done"),
            label("fail"),
            assertion(7),
            label("done"),
        },
    }};
}

model::Program same_address_fifo_program() {
    return model::Program{{
        {
            write("x", 1),
            write("x", 2),
        },
        {
            cas("x", 2, 2, 0),
            bnz(0, "saw_two"),
            set(7, 1),
            bnz(7, "done"),
            label("saw_two"),
            cas("x", 1, 1, 1),
            bnz(1, "fail"),
            set(7, 1),
            bnz(7, "done"),
            label("fail"),
            assertion(7),
            label("done"),
        },
    }};
}

model::Program multi_address_lasso_program() {
    return model::Program{{
        {
            set(1, 1),
            label("spin"),
            write("x", 1),
            write("y", 1),
            bnz(1, "spin"),
        },
    }};
}

void plain_mp_relaxes_only_under_pso() {
    const auto results = explore_all_models(message_passing_program(false), 40, 200000);
    require_shape("MP SC", results[0], {true, false, false, false, false});
    require_shape("MP TSO", results[1], {true, false, false, false, false});
    require_shape("MP PSO", results[2], {true, false, false, true, false});
}

void fenced_mp_is_ordered_in_all_models() {
    const auto results = explore_all_models(message_passing_program(true), 40, 200000);
    require_shape("MP+fence SC", results[0], {true, false, false, false, false});
    require_shape("MP+fence TSO", results[1], {true, false, false, false, false});
    require_shape("MP+fence PSO", results[2], {true, false, false, false, false});
}

void store_buffering_relaxes_under_tso_and_pso() {
    const auto results = explore_all_models(store_buffering_program(), 80, 300000);
    require_shape("SB SC", results[0], {true, false, false, false, false});
    require_shape("SB TSO", results[1], {true, false, false, true, false});
    require_shape("SB PSO", results[2], {true, false, false, true, false});
}

void same_address_stores_remain_fifo() {
    const auto results = explore_all_models(same_address_fifo_program(), 40, 200000);
    require_shape("same-address SC", results[0], {true, false, false, false, false});
    require_shape("same-address TSO", results[1], {true, false, false, false, false});
    require_shape("same-address PSO", results[2], {true, false, false, false, false});
}

void multi_address_buffers_are_in_the_lasso_fingerprint() {
    const model::Program program = multi_address_lasso_program();
    const model::ModelChecker sc(program, 20, model::MemoryModel::SC);
    const model::Schedule sc_witness{
        {0, 0}, {0, 2}, {0, 3},
        {0, 4}, {0, 2}, {0, 3},
    };
    require_shape("lasso SC", sc.replay(sc_witness), {false, false, false, false, true});

    const model::ModelChecker tso(program, 20, model::MemoryModel::TSO);
    const model::Schedule tso_witness{
        {0, 0},
        {0, 2},
        {0, 3},
        {0, model::kFlushActionIndex},
        {0, model::kFlushActionIndex},
        {0, 4},
        {0, 2},
        {0, 3},
        {0, model::kFlushActionIndex},
        {0, model::kFlushActionIndex},
    };
    require_shape("lasso TSO", tso.replay(tso_witness), {false, false, false, false, true});

    const model::ModelChecker pso(program, 20, model::MemoryModel::PSO);
    const model::Schedule stem{
        {0, 0},
        {0, 2},
        {0, 3},
        {0, 4},
        pso_flush(0, 0),
        pso_flush(0, 1),
        {0, 2},
        {0, 3},
        {0, 4},
    };
    const model::Schedule cycle_prefix{
        {0, 2},
        {0, 3},
        {0, 4},
    };
    model::Schedule before_flushes = stem;
    before_flushes.insert(before_flushes.end(), cycle_prefix.begin(), cycle_prefix.end());
    if (pso.replay(before_flushes).first_nontermination.has_value()) {
        throw std::runtime_error("PSO lasso closed before either address buffer was restored");
    }

    model::Schedule before_second_flush = before_flushes;
    before_second_flush.push_back(pso_flush(0, 0)); // canonical address 0 is x
    if (pso.replay(before_second_flush).first_nontermination.has_value()) {
        throw std::runtime_error("PSO lasso closed before the y buffer was restored");
    }

    model::Schedule witness = before_second_flush;
    witness.push_back(pso_flush(0, 1)); // canonical address 1 is y
    const auto replayed = pso.replay(witness);
    require_shape("lasso PSO", replayed, {false, false, false, false, true});
    if (replayed.first_nontermination->stem != stem) {
        throw std::runtime_error("PSO lasso stem did not preserve the multi-address buffer state");
    }
    model::Schedule cycle = cycle_prefix;
    cycle.push_back(pso_flush(0, 0));
    cycle.push_back(pso_flush(0, 1));
    if (replayed.first_nontermination->cycle != cycle ||
        replayed.first_nontermination->schedule != witness) {
        throw std::runtime_error("PSO multi-address lasso did not replay identically");
    }
}

void replay_rejects_missing_or_wrong_pso_flush_address() {
    const model::Program program{{{write("x", 1)}}};
    const model::ModelChecker pso(program, 10, model::MemoryModel::PSO);
    for (const model::Schedule invalid : {
             model::Schedule{{0, 0}, {0, model::kFlushActionIndex}},
             model::Schedule{{0, 0}, pso_flush(0, 1)},
         }) {
        try {
            (void)pso.replay(invalid);
            throw std::runtime_error("invalid PSO flush schedule was accepted");
        } catch (const std::invalid_argument&) {
            // Exact replay requires the canonical address id present at this
            // state; omission and a different numeric id are both invalid.
        }
    }
}

void pso_forwards_newest_same_address_store() {
    const model::Program program{{
        {
            write("x", 1),
            write("x", 2),
            read("x", 0),
            assertion(0),
        },
    }};
    const model::ModelChecker pso(program, 10, model::MemoryModel::PSO);
    const model::Schedule schedule{
        {0, 0},
        {0, 1},
        {0, 2},
        {0, 3},
        pso_flush(0, 0),
        pso_flush(0, 0),
    };
    require_shape("PSO forwarding", pso.replay(schedule), {});
}

void pso_flush_independence_is_address_aware() {
    model::Program program;
    program.threads.resize(2);
    const model::ModelChecker checker(program, 10, model::MemoryModel::PSO);
    model::Action flush_x;
    flush_x.kind = model::ActionKind::Flush;
    flush_x.address = "x";
    model::Action flush_y = flush_x;
    flush_y.address = "y";

    if (!checker.dpor_transitions_independent(0, flush_x, 0, flush_y) ||
        checker.dpor_transitions_independent(0, flush_x, 0, flush_x) ||
        !checker.dpor_transitions_independent(0, flush_x, 1, flush_y) ||
        checker.dpor_transitions_independent(0, flush_x, 1, flush_x)) {
        throw std::runtime_error("PSO flush independence was not address-aware");
    }
}

} // namespace

int main() {
    plain_mp_relaxes_only_under_pso();
    fenced_mp_is_ordered_in_all_models();
    store_buffering_relaxes_under_tso_and_pso();
    same_address_stores_remain_fifo();
    multi_address_buffers_are_in_the_lasso_fingerprint();
    replay_rejects_missing_or_wrong_pso_flush_address();
    pso_forwards_newest_same_address_store();
    pso_flush_independence_is_address_aware();
    std::cout << "pso_litmus: cases=5 models=3 MP=sc:no,tso:no,pso:yes"
                 " MP_fenced=sc:no,tso:no,pso:no SB=sc:no,tso:yes,pso:yes"
                 " same_address=sc:no,tso:no,pso:no lasso=sc:yes,tso:yes,pso:yes\n";
    return 0;
}
