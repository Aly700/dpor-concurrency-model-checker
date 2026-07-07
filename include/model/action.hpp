#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace model {

using ThreadId = std::uint32_t;

enum class ActionKind {
    Read,
    Write,
    AtomicLoad,
    AtomicStore,
    AtomicRmw,
    Lock,
    Unlock,
    Spawn,
    Join,
    Wait,
    Signal,
    Broadcast,
    Yield
};

struct Action {
    ActionKind kind{ActionKind::Yield};
    std::string address;
    std::string mutex;
    std::string condition;
    ThreadId target{0};

    bool operator==(const Action&) const = default;
};

struct Program {
    std::vector<std::vector<Action>> threads;
};

struct ScheduleStep {
    ThreadId thread{0};
    std::uint32_t action_index{0};

    bool operator==(const ScheduleStep&) const = default;
};

using Schedule = std::vector<ScheduleStep>;

bool may_conflict(const Action& lhs, const Action& rhs);
bool independent(const Action& lhs, const Action& rhs);

} // namespace model
