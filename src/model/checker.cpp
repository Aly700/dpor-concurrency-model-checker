#include "model/checker.hpp"

#include <stdexcept>
#include <utility>

namespace model {

ModelChecker::ModelChecker(Program program) : program_(std::move(program)) {}

CheckResult ModelChecker::explore_naive(std::size_t max_schedules) const {
    CheckResult result;
    std::vector<std::uint32_t> pc(program_.threads.size(), 0);
    Schedule schedule;
    dfs(pc, schedule, result, max_schedules);
    return result;
}

void ModelChecker::dfs(std::vector<std::uint32_t>& pc,
                       Schedule& schedule,
                       CheckResult& result,
                       std::size_t max_schedules) const {
    if (result.schedules_explored >= max_schedules) {
        return;
    }

    bool any_enabled = false;
    for (std::uint32_t tid = 0; tid < program_.threads.size(); ++tid) {
        if (pc[tid] >= program_.threads[tid].size()) {
            continue;
        }
        any_enabled = true;
        const auto action_index = pc[tid]++;
        schedule.push_back(ScheduleStep{tid, action_index});
        dfs(pc, schedule, result, max_schedules);
        schedule.pop_back();
        --pc[tid];
    }

    if (!any_enabled) {
        ++result.schedules_explored;
        if (!result.first_race.has_value()) {
            result.first_race = detect_adjacent_conflict(schedule);
        }
    }
}

std::optional<RaceReport> ModelChecker::detect_adjacent_conflict(const Schedule& schedule) const {
    for (std::size_t i = 1; i < schedule.size(); ++i) {
        const auto& prev = schedule[i - 1];
        const auto& cur = schedule[i];
        if (prev.thread == cur.thread) {
            continue;
        }
        const auto& a = program_.threads.at(prev.thread).at(prev.action_index);
        const auto& b = program_.threads.at(cur.thread).at(cur.action_index);
        if (may_conflict(a, b)) {
            return RaceReport{a.address, schedule};
        }
    }
    return std::nullopt;
}

} // namespace model
