#include "model/checker.hpp"

#include <cassert>
#include <iostream>
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

model::Action assert_zero() {
    model::Action action;
    action.kind = model::ActionKind::Assert;
    action.source_register = 7;
    return action;
}

model::Action assert_nonzero(model::RegisterId reg) {
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

model::Program store_buffering_program(bool fenced) {
    std::vector<model::Action> t0 = {
        write("x", 1),
    };
    if (fenced) {
        t0.push_back(fence());
    }
    t0.push_back(read("y", 0));
    t0.push_back(bnz(0, "done"));
    t0.push_back(atomic_store("t0_saw_zero", 1));
    t0.push_back(label("done"));

    std::vector<model::Action> t1 = {
        write("y", 1),
    };
    if (fenced) {
        t1.push_back(fence());
    }
    t1.push_back(read("x", 0));
    t1.push_back(bnz(0, "done"));
    t1.push_back(atomic_store("t1_saw_zero", 1));
    t1.push_back(label("done"));

    return model::Program{{
        std::move(t0),
        std::move(t1),
        {
            join(0),
            join(1),
            atomic_load("t0_saw_zero", 0),
            bnz(0, "check_other"),
            set(6, 1),
            bnz(6, "done"),
            label("check_other"),
            atomic_load("t1_saw_zero", 1),
            bnz(1, "fail"),
            set(6, 1),
            bnz(6, "done"),
            label("fail"),
            assert_zero(),
            label("done"),
        },
    }};
}

bool has_bug_shape(const model::CheckResult& result,
                   bool race,
                   bool deadlock,
                   bool error,
                   bool assertion) {
    return result.first_race.has_value() == race &&
           result.first_deadlock.has_value() == deadlock &&
           result.first_error.has_value() == error &&
           result.first_assertion.has_value() == assertion;
}

void require_result_shape(const char* label,
                          const model::CheckResult& result,
                          bool race,
                          bool deadlock,
                          bool error,
                          bool assertion) {
    if (!has_bug_shape(result, race, deadlock, error, assertion)) {
        std::cerr << label
                  << " race=" << result.first_race.has_value()
                  << " deadlock=" << result.first_deadlock.has_value()
                  << " error=" << result.first_error.has_value()
                  << " assertion=" << result.first_assertion.has_value()
                  << " schedules=" << result.schedules_explored << '\n';
        assert(false && "unexpected litmus result shape");
    }
}

void store_buffering_relaxed_outcome_is_tso_only() {
    const model::Program program = store_buffering_program(false);
    const model::ModelChecker sc(program, 80, model::MemoryModel::SC);
    const model::ModelChecker tso(program, 80, model::MemoryModel::TSO);

    const auto sc_result = sc.explore_dpor(200000);
    const auto tso_result = tso.explore_dpor(200000);

    require_result_shape("SB SC", sc_result, true, false, false, false);
    require_result_shape("SB TSO", tso_result, true, false, false, true);
    assert(tso.replay(tso_result.first_assertion->schedule).first_assertion == tso_result.first_assertion);
}

void store_buffering_fences_restore_sc_observable_outcome() {
    const model::Program program = store_buffering_program(true);
    const model::ModelChecker sc(program, 80, model::MemoryModel::SC);
    const model::ModelChecker tso(program, 80, model::MemoryModel::TSO);

    const auto sc_result = sc.explore_dpor(200000);
    const auto tso_result = tso.explore_dpor(200000);

    require_result_shape("SB+fence SC", sc_result, true, false, false, false);
    require_result_shape("SB+fence TSO", tso_result, true, false, false, false);
}

void store_to_load_forwarding_is_observable() {
    const model::Program program{{
        {
            write("x", 1),
            read("x", 0),
            assert_nonzero(0),
        },
    }};
    const model::ModelChecker sc_checker(program, 20, model::MemoryModel::SC);
    const model::ModelChecker checker(program, 20, model::MemoryModel::TSO);

    require_result_shape("forwarding SC", sc_checker.explore_dpor(100), false, false, false, false);

    const auto replay = checker.replay({
        {0, 0},
        {0, 1},
        {0, model::kFlushActionIndex},
    });
    require_result_shape("forwarding replay", replay, false, false, false, false);

    const auto dpor = checker.explore_dpor(100);
    require_result_shape("forwarding TSO", dpor, false, false, false, false);
}

void plain_message_passing_flag_can_stay_stale_until_flush() {
    const model::Program program{{
        {
            write("data", 1),
            write("flag", 1),
        },
        {
            read("flag", 0),
            bnz(0, "saw_flag"),
            atomic_store("saw_stale_flag", 1),
            label("saw_flag"),
        },
        {
            join(0),
            join(1),
            atomic_load("saw_stale_flag", 0),
            bnz(0, "fail"),
            set(6, 1),
            bnz(6, "done"),
            label("fail"),
            assert_zero(),
            label("done"),
        },
    }};

    const model::ModelChecker sc_checker(program, 80, model::MemoryModel::SC);
    const model::ModelChecker tso_checker(program, 80, model::MemoryModel::TSO);
    require_result_shape("plain MP SC", sc_checker.explore_dpor(200000), true, false, false, true);
    require_result_shape("plain MP TSO", tso_checker.explore_dpor(200000), true, false, false, true);
}

} // namespace

int main() {
    store_buffering_relaxed_outcome_is_tso_only();
    store_buffering_fences_restore_sc_observable_outcome();
    store_to_load_forwarding_is_observable();
    plain_message_passing_flag_can_stay_stale_until_flush();
    return 0;
}
