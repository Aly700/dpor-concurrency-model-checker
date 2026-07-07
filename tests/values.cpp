#include "model/action.hpp"
#include "model/checker.hpp"

#include <cassert>
#include <string>
#include <utility>

namespace {

model::ValueOperand imm(model::Value value) {
    model::ValueOperand operand;
    operand.kind = model::ValueOperandKind::Immediate;
    operand.immediate = value;
    return operand;
}

model::ValueOperand reg(model::RegisterId reg_id) {
    model::ValueOperand operand;
    operand.kind = model::ValueOperandKind::Register;
    operand.reg = reg_id;
    return operand;
}

model::Action set(model::RegisterId reg_id, model::Value value) {
    model::Action action;
    action.kind = model::ActionKind::Set;
    action.destination = reg_id;
    action.value = imm(value);
    return action;
}

model::Action label(std::string name) {
    model::Action action;
    action.kind = model::ActionKind::Label;
    action.label = std::move(name);
    return action;
}

model::Action bnz(model::RegisterId reg_id, std::string target) {
    model::Action action;
    action.kind = model::ActionKind::BranchNonzero;
    action.source_register = reg_id;
    action.label = std::move(target);
    return action;
}

model::Action assert_nonzero(model::RegisterId reg_id) {
    model::Action action;
    action.kind = model::ActionKind::Assert;
    action.source_register = reg_id;
    return action;
}

model::Action read(std::string address, model::RegisterId reg_id) {
    model::Action action;
    action.kind = model::ActionKind::Read;
    action.address = std::move(address);
    action.destination = reg_id;
    return action;
}

model::Action write(std::string address, model::ValueOperand value) {
    model::Action action;
    action.kind = model::ActionKind::Write;
    action.address = std::move(address);
    action.value = value;
    return action;
}

model::Action atomic_load(std::string address, model::RegisterId reg_id) {
    model::Action action;
    action.kind = model::ActionKind::AtomicLoad;
    action.address = std::move(address);
    action.destination = reg_id;
    return action;
}

model::Action atomic_store(std::string address, model::ValueOperand value) {
    model::Action action;
    action.kind = model::ActionKind::AtomicStore;
    action.address = std::move(address);
    action.value = value;
    return action;
}

model::Action atomic_rmw(std::string address, model::ValueOperand value, model::RegisterId result) {
    model::Action action;
    action.kind = model::ActionKind::AtomicRmw;
    action.address = std::move(address);
    action.value = value;
    action.destination = result;
    return action;
}

model::Action cas(std::string address,
                  model::ValueOperand expected,
                  model::ValueOperand desired,
                  model::RegisterId result) {
    model::Action action;
    action.kind = model::ActionKind::CompareExchange;
    action.address = std::move(address);
    action.expected = expected;
    action.value = desired;
    action.destination = result;
    return action;
}

model::Action join(model::ThreadId target) {
    model::Action action;
    action.kind = model::ActionKind::Join;
    action.target = target;
    return action;
}

void register_only_actions_reduce_to_one_trace_class() {
    const model::Program program{{
        {set(0, 1), set(1, 2), bnz(7, "done"), set(2, 3), label("done"), assert_nonzero(0)},
        {set(0, 4), set(1, 5), assert_nonzero(1)},
    }};
    const model::ModelChecker checker(program);

    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();

    assert(naive.schedules_explored > 1);
    assert(dpor.schedules_explored == 1);
    assert(!dpor.first_race.has_value());
    assert(!dpor.first_deadlock.has_value());
    assert(!dpor.first_error.has_value());
    assert(!dpor.first_assertion.has_value());
    assert(dpor.bound_exceeded_executions == 0);
}

void failed_cas_is_acquire_only_and_does_not_publish_prior_plain_write() {
    const model::Program program{{
        {atomic_store("f", imm(1))},
        {write("x", imm(1)), cas("f", imm(0), imm(2), 0)},
        {atomic_load("f", 0), write("x", imm(2))},
    }};
    const model::ModelChecker checker(program);

    const auto replay = checker.replay({{0, 0}, {1, 0}, {1, 1}, {2, 0}, {2, 1}});
    assert(replay.first_race.has_value());
    assert(replay.first_race->address == "x");
}

void successful_cas_publishes_prior_plain_write_to_later_acquire_load() {
    const model::Program program{{
        {write("x", imm(1)), cas("f", imm(0), imm(1), 0)},
        {atomic_load("f", 0), read("x", 1)},
    }};
    const model::ModelChecker checker(program);

    const auto replay = checker.replay({{0, 0}, {0, 1}, {1, 0}, {1, 1}});
    assert(!replay.first_race.has_value());
    assert(!replay.first_deadlock.has_value());
    assert(!replay.first_error.has_value());
    assert(!replay.first_assertion.has_value());
}

void fetch_add_returns_old_value_and_two_joined_increments_sum_to_two() {
    const model::Program return_old{{
        {
            atomic_rmw("f", imm(1), 0),
            bnz(0, "bad_old_zero"),
            atomic_rmw("f", imm(1), 0),
            assert_nonzero(0),
            set(1, 1),
            bnz(1, "done"),
            label("bad_old_zero"),
            assert_nonzero(7),
            label("done"),
        },
    }};
    const model::ModelChecker return_old_checker(return_old);
    const auto return_old_result = return_old_checker.explore_naive();
    assert(!return_old_result.first_assertion.has_value());

    const model::Program sum_to_two{{
        {atomic_rmw("f", imm(1), 0)},
        {atomic_rmw("f", imm(1), 0)},
        {join(0), join(1), cas("f", imm(2), imm(2), 0), assert_nonzero(0)},
    }};
    const model::ModelChecker sum_checker(sum_to_two);
    const auto naive = sum_checker.explore_naive();
    const auto dpor = sum_checker.explore_dpor();
    assert(!naive.first_assertion.has_value());
    assert(!dpor.first_assertion.has_value());
    assert(naive.first_race.has_value() == dpor.first_race.has_value());
    assert(naive.bound_exceeded_executions == 0);
    assert(dpor.bound_exceeded_executions == 0);
}

void assertion_failure_is_minimized_and_replayable() {
    const model::Program program{{
        {assert_nonzero(0)},
        {set(0, 1), assert_nonzero(0)},
    }};
    const model::ModelChecker checker(program);

    const auto dpor = checker.explore_dpor();
    assert(dpor.first_assertion.has_value());
    assert(dpor.first_assertion->endpoint == (model::ScheduleStep{0, 0}));
    assert(dpor.first_assertion->value == 0);
    assert(dpor.first_assertion->schedule == (model::Schedule{{0, 0}}));

    const auto replay = checker.replay(dpor.first_assertion->schedule);
    assert(replay.first_assertion.has_value());
    assert(*replay.first_assertion == *dpor.first_assertion);
}

void spin_loop_reports_bound_exceeded_but_completing_schedules_are_clean() {
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
        {atomic_store("f", imm(1))},
    }};
    const model::ModelChecker checker(program, 5);

    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();

    assert(!naive.first_race.has_value());
    assert(!dpor.first_race.has_value());
    assert(!naive.first_deadlock.has_value());
    assert(!dpor.first_deadlock.has_value());
    assert(!naive.first_error.has_value());
    assert(!dpor.first_error.has_value());
    assert(!naive.first_assertion.has_value());
    assert(!dpor.first_assertion.has_value());
    assert(naive.bound_exceeded_executions > 0);
    assert(dpor.bound_exceeded_executions > 0);
}

} // namespace

int main() {
    register_only_actions_reduce_to_one_trace_class();
    failed_cas_is_acquire_only_and_does_not_publish_prior_plain_write();
    successful_cas_publishes_prior_plain_write_to_later_acquire_load();
    fetch_add_returns_old_value_and_two_joined_increments_sum_to_two();
    assertion_failure_is_minimized_and_replayable();
    spin_loop_reports_bound_exceeded_but_completing_schedules_are_clean();
    return 0;
}
