#pragma once

#include "model/action.hpp"

#include <optional>
#include <string>
#include <vector>

namespace model {

struct RaceReport {
    std::string address;
    Schedule schedule;
};

struct CheckResult {
    std::size_t schedules_explored{0};
    std::optional<RaceReport> first_race;
};

class ModelChecker {
public:
    explicit ModelChecker(Program program);
    CheckResult explore_naive(std::size_t max_schedules = 100000) const;

private:
    void dfs(std::vector<std::uint32_t>& pc,
             Schedule& schedule,
             CheckResult& result,
             std::size_t max_schedules) const;
    [[nodiscard]] std::optional<RaceReport> detect_adjacent_conflict(const Schedule& schedule) const;

    Program program_;
};

} // namespace model
