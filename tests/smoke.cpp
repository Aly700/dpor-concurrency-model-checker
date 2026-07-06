#include "model/checker.hpp"
#include "model/vector_clock.hpp"

#include <cassert>

int main() {
    model::Program program;
    program.threads = {
        {{model::ActionKind::Write, "x", ""}},
        {{model::ActionKind::Write, "x", ""}},
    };

    model::ModelChecker checker(program);
    auto result = checker.explore_naive();
    assert(result.schedules_explored == 2);
    assert(result.first_race.has_value());
    assert(result.first_race->address == "x");

    model::VectorClock a;
    model::VectorClock b;
    a.tick(0);
    b.join(a);
    b.tick(1);
    assert(a.happens_before_or_equal(b));

    return 0;
}
