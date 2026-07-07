#include "model/action.hpp"
#include "model/checker.hpp"

#include <cassert>
#include <string>
#include <vector>

namespace {

model::Action read(std::string address) {
    return model::Action{model::ActionKind::Read, std::move(address), ""};
}

model::Action write(std::string address) {
    return model::Action{model::ActionKind::Write, std::move(address), ""};
}

model::Action lock(std::string mutex) {
    return model::Action{model::ActionKind::Lock, "", std::move(mutex)};
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

} // namespace

int main() {
    assert(!model::independent(write("x"), write("x")));
    assert(!model::independent(write("x"), read("x")));
    assert(!model::independent(lock("m"), lock("m")));
    assert(!model::independent(join(0), yield()));
    assert(!model::independent(spawn(1), yield()));
    assert(!model::independent(wait("cv", "m"), signal("cv")));
    assert(!model::independent(wait("cv", "m"), broadcast("cv")));
    assert(!model::independent(wait("cv0", "m"), lock("m")));
    assert(model::independent(wait("cv0", "m"), signal("cv1")));

    assert_pair_commutes_when_independent(read("x"), read("x"));
    assert_pair_commutes_when_independent(read("x"), write("y"));
    assert_pair_commutes_when_independent(write("x"), lock("m"));
    assert_pair_commutes_when_independent(lock("m"), lock("n"));
    assert_pair_commutes_when_independent(signal("cv0"), signal("cv1"));
    assert_pair_commutes_when_independent(broadcast("cv0"), signal("cv1"));
    assert_pair_commutes_when_independent(yield(), write("x"));

    return 0;
}
