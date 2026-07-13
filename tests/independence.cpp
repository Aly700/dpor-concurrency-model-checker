#include "model/action.hpp"
#include "model/checker.hpp"

#include <array>
#include <cassert>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

model::Action read(std::string address) {
    return model::Action{model::ActionKind::Read, std::move(address), ""};
}

model::Action read_into(std::string address, model::RegisterId reg) {
    model::Action action = read(std::move(address));
    action.destination = reg;
    return action;
}

model::Action write(std::string address) {
    return model::Action{model::ActionKind::Write, std::move(address), ""};
}

model::Action write_value(std::string address, model::Value value) {
    model::Action action = write(std::move(address));
    model::ValueOperand operand;
    operand.kind = model::ValueOperandKind::Immediate;
    operand.immediate = value;
    action.value = operand;
    return action;
}

model::Action lock(std::string mutex) {
    return model::Action{model::ActionKind::Lock, "", std::move(mutex)};
}

model::Action rwlock_action(model::ActionKind kind, std::string rwlock) {
    model::Action action;
    action.kind = kind;
    action.rwlock = std::move(rwlock);
    return action;
}

model::Action rlock(std::string rwlock) {
    return rwlock_action(model::ActionKind::RLock, std::move(rwlock));
}

model::Action runlock(std::string rwlock) {
    return rwlock_action(model::ActionKind::RUnlock, std::move(rwlock));
}

model::Action wlock(std::string rwlock) {
    return rwlock_action(model::ActionKind::WLock, std::move(rwlock));
}

model::Action wunlock(std::string rwlock) {
    return rwlock_action(model::ActionKind::WUnlock, std::move(rwlock));
}

model::Action join(model::ThreadId target) {
    model::Action action;
    action.kind = model::ActionKind::Join;
    action.target = target;
    return action;
}

model::Action spawn(model::ThreadId target) {
    model::Action action;
    action.kind = model::ActionKind::Spawn;
    action.target = target;
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

model::Action broadcast(std::string condition) {
    model::Action action;
    action.kind = model::ActionKind::Broadcast;
    action.condition = std::move(condition);
    return action;
}

model::Action yield() {
    return model::Action{model::ActionKind::Yield, "", ""};
}

model::Action flush(std::string address) {
    model::Action action;
    action.kind = model::ActionKind::Flush;
    action.address = std::move(address);
    return action;
}

model::Action set(model::RegisterId reg, model::Value value) {
    model::Action action;
    action.kind = model::ActionKind::Set;
    action.destination = reg;
    model::ValueOperand operand;
    operand.kind = model::ValueOperandKind::Immediate;
    operand.immediate = value;
    action.value = operand;
    return action;
}

model::Action assert_nonzero(model::RegisterId reg) {
    model::Action action;
    action.kind = model::ActionKind::Assert;
    action.source_register = reg;
    return action;
}

void assert_pair_commutes_when_independent(const model::Action& lhs, const model::Action& rhs) {
    assert(model::independent(lhs, rhs));

    model::Program program;
    program.threads = {{lhs}, {rhs}};
    const model::ModelChecker checker(program);

    const auto explored = checker.explore_naive();
    assert(explored.schedules_explored == 2);
    assert(!explored.first_race.has_value());
    assert(!explored.first_deadlock.has_value());
    assert(!explored.first_error.has_value());

    const auto lhs_then_rhs = checker.replay({{0, 0}, {1, 0}});
    const auto rhs_then_lhs = checker.replay({{1, 0}, {0, 0}});
    assert(!lhs_then_rhs.first_race.has_value());
    assert(!lhs_then_rhs.first_deadlock.has_value());
    assert(!lhs_then_rhs.first_error.has_value());
    assert(!rhs_then_lhs.first_race.has_value());
    assert(!rhs_then_lhs.first_deadlock.has_value());
    assert(!rhs_then_lhs.first_error.has_value());
}

void assert_buffered_enqueue_reduction(model::MemoryModel memory_model) {
    const model::Program program{{
        {write("y")},
        {read("x"), read("y")},
    }};
    const model::ModelChecker checker(program, 20, memory_model);

    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    require(naive.schedules_explored == 6,
            "buffered enqueue reduction fixture changed its naive schedule count");
    require(dpor.schedules_explored == 3,
            "buffered source Write did not reduce as a private enqueue");
    require(naive.first_race.has_value(),
            "buffered enqueue reduction fixture lost its naive race");
    require(dpor.first_race.has_value(),
            "buffered enqueue reduction fixture lost its DPOR race");
}

void assert_flush_visibility_dependency_preserves_assertion(model::MemoryModel memory_model) {
    const model::Program program{{
        {write_value("x", 1)},
        {read_into("x", 0), assert_nonzero(0)},
    }};
    const model::ModelChecker checker(program, 20, memory_model);

    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    require(naive.first_assertion.has_value(),
            "flush visibility trap lost its naive assertion witness");
    require(dpor.first_assertion.has_value(),
            "flush visibility dependency was pruned and lost the assertion witness");
}

} // namespace

int main() {
    assert(!model::independent(write("x"), write("x")));
    assert(!model::independent(write("x"), read("x")));
    assert(!model::independent(flush("x"), read("x")));
    assert(!model::independent(flush("x"), write("x")));
    assert(!model::independent(flush("x"), flush("x")));
    assert(!model::independent(lock("m"), lock("m")));
    assert(!model::independent(join(0), yield()));
    assert(!model::independent(spawn(1), yield()));
    assert(!model::independent(wait("cv", "m"), signal("cv")));
    assert(!model::independent(wait("cv", "m"), broadcast("cv")));
    assert(!model::independent(wait("cv0", "m"), lock("m")));
    assert(model::independent(wait("cv0", "m"), signal("cv1")));
    assert(model::independent(set(0, 1), write("x")));
    assert(model::independent(assert_nonzero(0), spawn(1)));
    assert(model::independent(flush("x"), read("y")));
    assert(model::independent(flush("x"), set(0, 1)));

    const std::array<model::Action, 4> same_rwlock_actions{
        rlock("rw"), runlock("rw"), wlock("rw"), wunlock("rw")};
    for (std::size_t lhs = 0; lhs < same_rwlock_actions.size(); ++lhs) {
        for (std::size_t rhs = 0; rhs < same_rwlock_actions.size(); ++rhs) {
            const bool reader_reader_acquires = lhs == 0 && rhs == 0;
            require(model::independent(
                        same_rwlock_actions.at(lhs), same_rwlock_actions.at(rhs)) ==
                        reader_reader_acquires,
                    "same-rwlock action-level independence matrix changed");
        }
    }
    const std::array<model::Action, 4> other_rwlock_actions{
        rlock("other"), runlock("other"), wlock("other"), wunlock("other")};
    for (const model::Action& lhs : same_rwlock_actions) {
        for (const model::Action& rhs : other_rwlock_actions) {
            require(model::independent(lhs, rhs),
                    "different-rwlock actions must remain independent");
        }
    }

    const model::Program relation_program{{
        {write("x")},
        {read("x")},
    }};
    const model::ModelChecker sc_checker(relation_program, 20, model::MemoryModel::SC);
    const model::ModelChecker tso_checker(relation_program, 20, model::MemoryModel::TSO);
    const model::ModelChecker pso_checker(relation_program, 20, model::MemoryModel::PSO);
    require(!sc_checker.dpor_transitions_independent(0, write("x"), 1, read("x")),
            "SC source Write/Read dependence changed");
    require(tso_checker.dpor_transitions_independent(0, write("x"), 1, read("x")),
            "TSO source Write was not classified as a private enqueue");
    require(pso_checker.dpor_transitions_independent(0, write("x"), 1, read("x")),
            "PSO source Write was not classified as a private enqueue");
    require(!tso_checker.dpor_transitions_independent(0, flush("x"), 1, read("x")),
            "TSO Flush/Read visibility dependence changed");
    require(!pso_checker.dpor_transitions_independent(0, flush("x"), 1, flush("x")),
            "PSO same-address Flush/Flush dependence changed");
    require(!tso_checker.dpor_transitions_independent(0, write("x"), 0, flush("x")),
            "same-thread source/Flush ordering changed");
    require(!tso_checker.dpor_transitions_independent(0, write("x"), 1, spawn(0)),
            "buffered enqueue bypassed the Spawn safeguard");
    require(!tso_checker.dpor_transitions_independent(0, write("x"), 1, join(0)),
            "buffered enqueue bypassed the target-Join safeguard");

    assert_buffered_enqueue_reduction(model::MemoryModel::TSO);
    assert_buffered_enqueue_reduction(model::MemoryModel::PSO);
    assert_flush_visibility_dependency_preserves_assertion(model::MemoryModel::TSO);
    assert_flush_visibility_dependency_preserves_assertion(model::MemoryModel::PSO);

    assert_pair_commutes_when_independent(read("x"), read("x"));
    assert_pair_commutes_when_independent(read("x"), write("y"));
    assert_pair_commutes_when_independent(write("x"), lock("m"));
    assert_pair_commutes_when_independent(lock("m"), lock("n"));
    assert_pair_commutes_when_independent(rlock("rw"), rlock("rw"));
    assert_pair_commutes_when_independent(signal("cv0"), signal("cv1"));
    assert_pair_commutes_when_independent(broadcast("cv0"), signal("cv1"));
    assert_pair_commutes_when_independent(yield(), write("x"));

    return 0;
}
