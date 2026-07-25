#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace model {

using ThreadId = std::uint32_t;
using RegisterId = std::uint8_t;
using Value = std::int64_t;

inline constexpr std::size_t kRegisterCount = 8;
inline constexpr std::uint32_t kFlushActionIndex = std::numeric_limits<std::uint32_t>::max();

enum class ValueOperandKind { Immediate, Register };

struct ValueOperand {
    ValueOperandKind kind{ValueOperandKind::Immediate};
    Value immediate{0};
    RegisterId reg{0};

    bool operator==(const ValueOperand&) const = default;
};

enum class ActionKind {
    Set,
    Label,
    BranchNonzero,
    Assert,
    Read,
    Write,
    AtomicLoad,
    AtomicStore,
    AtomicRmw,
    CompareExchange,
    Fence,
    Flush,
    Lock,
    TryLock,
    Unlock,
    Spawn,
    Join,
    Wait,
    Signal,
    Broadcast,
    Yield,
    RLock,
    RUnlock,
    WLock,
    WUnlock,
    SemPost,
    SemWait,
    BarrierWait,
    Upgrade,
    Downgrade
};

struct Action {
    ActionKind kind{ActionKind::Yield};
    std::string address;
    std::string mutex;
    std::string rwlock;
    std::string semaphore;
    std::string condition;
    ThreadId target{0};
    std::optional<RegisterId> destination;
    std::optional<RegisterId> source_register;
    std::optional<ValueOperand> value;
    std::optional<ValueOperand> expected;
    std::string label;
    // Kept at the end so existing aggregate initializers for older action
    // kinds retain their field positions.
    std::string barrier;
    std::uint32_t parties{0};

    bool operator==(const Action&) const = default;
};

struct Program {
    std::vector<std::vector<Action>> threads;
};

struct ScheduleStep {
    ThreadId thread{0};
    std::uint32_t action_index{0};
    // Internal PSO flushes retain kFlushActionIndex and add the canonical
    // numeric program-address id here. Source actions and TSO flushes leave it
    // empty, preserving the existing two-number schedule representation.
    std::optional<std::uint32_t> flush_address;

    bool operator==(const ScheduleStep&) const = default;
    bool operator<(const ScheduleStep& other) const {
        if (thread != other.thread) {
            return thread < other.thread;
        }
        if (action_index != other.action_index) {
            return action_index < other.action_index;
        }
        return flush_address < other.flush_address;
    }
};

using Schedule = std::vector<ScheduleStep>;

bool may_conflict(const Action& lhs, const Action& rhs);
bool independent(const Action& lhs, const Action& rhs);

} // namespace model
