#include "model/checker.hpp"

#include <cassert>
#include <optional>
#include <vector>

namespace {

model::ValueOperand immediate(model::Value value) {
    model::ValueOperand operand;
    operand.kind = model::ValueOperandKind::Immediate;
    operand.immediate = value;
    return operand;
}

model::Action write_value(const char* address, model::Value value) {
    model::Action action;
    action.kind = model::ActionKind::Write;
    action.address = address;
    action.value = immediate(value);
    return action;
}

void collects_exact_dpor_representatives() {
    const model::Program program{{
        {write_value("x", 1)},
        {write_value("x", 2)},
    }};
    const model::ModelChecker checker(program, 4, model::MemoryModel::SC);

    const std::vector<model::Schedule> schedules = checker.collect_dpor_schedules();

    assert(schedules == (std::vector<model::Schedule>{
        {{0, 0, std::nullopt}, {1, 0, std::nullopt}},
        {{1, 0, std::nullopt}, {0, 0, std::nullopt}},
    }));
    assert(schedules.size() == checker.explore_dpor().schedules_explored);
}

void inspection_exposes_enabledness_effects_and_incomparable_clocks() {
    const model::Program program{{
        {write_value("x", 1)},
        {write_value("x", 2)},
    }};
    const model::ModelChecker checker(program, 4, model::MemoryModel::SC);
    const model::Schedule schedule{
        {0, 0, std::nullopt},
        {1, 0, std::nullopt},
    };

    const std::vector<model::InspectedScheduleStep> trace =
        checker.inspect_schedule(schedule);

    assert(trace.size() == 2);
    assert(trace.at(0).enabled_before == (std::vector<model::ScheduleStep>{
        {0, 0, std::nullopt}, {1, 0, std::nullopt}}));
    assert(trace.at(0).clock_after == (std::vector<std::uint64_t>{1, 0}));
    assert(trace.at(1).clock_after == (std::vector<std::uint64_t>{0, 1}));

    assert(trace.at(0).memory_effects.size() == 1);
    assert(trace.at(0).memory_effects.at(0).address == "x");
    assert(trace.at(0).memory_effects.at(0).before == std::optional<model::Value>{0});
    assert(trace.at(0).memory_effects.at(0).after == std::optional<model::Value>{1});
    assert(trace.at(1).memory_effects.size() == 1);
    assert(trace.at(1).memory_effects.at(0).before == std::optional<model::Value>{1});
    assert(trace.at(1).memory_effects.at(0).after == std::optional<model::Value>{2});
}

} // namespace

int main() {
    collects_exact_dpor_representatives();
    inspection_exposes_enabledness_effects_and_incomparable_clocks();
    return 0;
}
