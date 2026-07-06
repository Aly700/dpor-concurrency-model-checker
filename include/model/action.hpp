#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace model {

using ThreadId = std::uint32_t;

enum class ActionKind { Read, Write, Lock, Unlock, Yield };

struct Action {
    ActionKind kind{ActionKind::Yield};
    std::string address;
    std::string mutex;
};

struct Program {
    std::vector<std::vector<Action>> threads;
};

struct ScheduleStep {
    ThreadId thread{0};
    std::uint32_t action_index{0};
};

using Schedule = std::vector<ScheduleStep>;

bool may_conflict(const Action& lhs, const Action& rhs);

} // namespace model
