#include "model/checker.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kMaxMeteredNaiveSchedules = 24;
constexpr std::size_t kNoScheduleCap = std::numeric_limits<std::size_t>::max();

enum class ProgramSource { TwoThreadFamily, HandPicked, Fuzz };
enum class VerdictKind { Clean, Race, Deadlock, Error, Assertion, NonTermination, Bound };
enum class ClassRelation { Current, OriginalBufferedWriteVisibility };

struct SourceStats {
    std::size_t metered{0};
};

struct AggregateStats {
    SourceStats two_thread;
    SourceStats hand_picked;
    SourceStats fuzz;
    std::size_t programs_metered{0};
    std::size_t total_classes{0};
    std::size_t total_original_relation_classes{0};
    std::size_t total_dpor_schedules{0};
    std::size_t total_naive_schedules{0};
    std::size_t optimal_programs{0};
};

bool same_thread_steps_ordered(model::MemoryModel memory_model,
                               const model::EffectiveScheduleStep& lhs,
                               const model::EffectiveScheduleStep& rhs) {
    if (lhs.endpoint.thread != rhs.endpoint.thread) {
        return false;
    }

    const bool both_flushes =
        lhs.effective_action.kind == model::ActionKind::Flush &&
        rhs.effective_action.kind == model::ActionKind::Flush;
    if (!both_flushes || memory_model != model::MemoryModel::PSO) {
        return true;
    }

    return lhs.effective_action.address == rhs.effective_action.address;
}

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

model::Action read(std::string address) {
    model::Action action;
    action.kind = model::ActionKind::Read;
    action.address = std::move(address);
    return action;
}

model::Action write(std::string address) {
    model::Action action;
    action.kind = model::ActionKind::Write;
    action.address = std::move(address);
    return action;
}

model::EffectiveScheduleStep effective_step(model::ThreadId thread,
                                            std::uint32_t action_index,
                                            model::Action action,
                                            std::optional<std::uint32_t> flush_address = std::nullopt) {
    return model::EffectiveScheduleStep{
        model::ScheduleStep{thread, action_index, flush_address},
        std::move(action),
    };
}

model::Action flush(std::string address) {
    model::Action action;
    action.kind = model::ActionKind::Flush;
    action.address = std::move(address);
    return action;
}

void test_same_thread_flush_correspondence() {
    const auto source_x = effective_step(0, 0, write("x"));
    const auto source_y = effective_step(0, 1, write("y"));
    const auto tso_flush_x = effective_step(0, model::kFlushActionIndex, flush("x"));
    const auto tso_flush_y = effective_step(0, model::kFlushActionIndex, flush("y"));
    const auto pso_flush_x = effective_step(0, model::kFlushActionIndex, flush("x"), 0);
    const auto pso_flush_x_again =
        effective_step(0, model::kFlushActionIndex, flush("x"), 0);
    const auto pso_flush_y = effective_step(0, model::kFlushActionIndex, flush("y"), 1);
    const auto other_thread_flush_y =
        effective_step(1, model::kFlushActionIndex, flush("y"), 1);

    require(same_thread_steps_ordered(model::MemoryModel::SC, source_x, source_y),
            "same-thread program actions must remain ordered");
    require(same_thread_steps_ordered(model::MemoryModel::TSO, source_x, tso_flush_x),
            "same-thread source/flush pairs must remain ordered");
    require(same_thread_steps_ordered(model::MemoryModel::TSO, tso_flush_x, tso_flush_y),
            "TSO same-thread flushes must remain single-FIFO ordered");
    require(same_thread_steps_ordered(
                model::MemoryModel::PSO, pso_flush_x, pso_flush_x_again),
            "PSO same-thread same-address flushes must remain FIFO ordered");
    require(!same_thread_steps_ordered(model::MemoryModel::PSO, pso_flush_x, pso_flush_y),
            "PSO same-thread different-address flushes must remain unordered");
    require(!same_thread_steps_ordered(
                model::MemoryModel::PSO, pso_flush_x, other_thread_flush_y),
            "same-thread ordering must not constrain different threads");
}

void test_replay_exposes_pso_flush_identity() {
    const model::Program program{{{write("x"), write("y")}}};
    const model::ModelChecker checker(program, 20, model::MemoryModel::PSO);
    const model::Schedule schedule{
        {0, 0, std::nullopt},
        {0, 1, std::nullopt},
        {0, model::kFlushActionIndex, 1},
        {0, model::kFlushActionIndex, 0},
    };
    const auto trace = checker.replay_effective_trace(schedule);
    require(trace.size() == schedule.size(), "PSO effective replay lost a flush transition");
    require(trace.at(2).endpoint.flush_address == 1 &&
                trace.at(2).effective_action.kind == model::ActionKind::Flush &&
                trace.at(2).effective_action.address == "y",
            "PSO flush(y) identity was not preserved by effective replay");
    require(trace.at(3).endpoint.flush_address == 0 &&
                trace.at(3).effective_action.kind == model::ActionKind::Flush &&
                trace.at(3).effective_action.address == "x",
            "PSO flush(x) identity was not preserved by effective replay");
}

std::size_t class_count_for(
    const model::Program& program,
    model::MemoryModel memory_model,
    ClassRelation relation = ClassRelation::Current);

void test_pso_flush_reordering_changes_class_count() {
    const model::Program program{{
        {write("x"), write("y")},
        {read("y"), read("x")},
    }};
    const std::size_t tso_classes = class_count_for(program, model::MemoryModel::TSO);
    const std::size_t pso_classes = class_count_for(program, model::MemoryModel::PSO);
    if (tso_classes != 6 || pso_classes != 7) {
        std::ostringstream message;
        message << "PSO different-address flush reordering class baseline changed: TSO="
                << tso_classes << " PSO=" << pso_classes;
        throw std::runtime_error(message.str());
    }
}

model::Action atomic_load(std::string address) {
    model::Action action;
    action.kind = model::ActionKind::AtomicLoad;
    action.address = std::move(address);
    return action;
}

model::Action atomic_store(std::string address) {
    model::Action action;
    action.kind = model::ActionKind::AtomicStore;
    action.address = std::move(address);
    return action;
}

model::Action atomic_rmw(std::string address) {
    model::Action action;
    action.kind = model::ActionKind::AtomicRmw;
    action.address = std::move(address);
    return action;
}

model::Action cas(std::string address) {
    model::Action action;
    action.kind = model::ActionKind::CompareExchange;
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

model::Action lock(std::string mutex) {
    model::Action action;
    action.kind = model::ActionKind::Lock;
    action.mutex = std::move(mutex);
    return action;
}

model::Action unlock(std::string mutex) {
    model::Action action;
    action.kind = model::ActionKind::Unlock;
    action.mutex = std::move(mutex);
    return action;
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
    model::Action action;
    action.kind = model::ActionKind::Yield;
    return action;
}

model::Action fence() {
    model::Action action;
    action.kind = model::ActionKind::Fence;
    return action;
}

const std::array<model::Action, 17> kTwoThreadActions{
    read("x"),
    write("x"),
    write("y"),
    atomic_load("f"),
    atomic_store("f"),
    atomic_rmw("f"),
    lock("m"),
    lock("n"),
    unlock("m"),
    unlock("n"),
    wait("cv", "m"),
    signal("cv"),
    broadcast("cv"),
    spawn(1),
    join(0),
    join(1),
    yield(),
};

// The buffered-model corpus is a small restriction of the common 11-action
// family used by tso_oracle and pso_oracle. It deliberately includes plain
// writes on two addresses so both concrete flush identities are exercised.
const std::array<model::Action, 11> kBufferedActions{
    read("x"),
    read("y"),
    write("x"),
    write("y"),
    atomic_load("x"),
    atomic_load("y"),
    atomic_store("x"),
    atomic_store("y"),
    lock("m"),
    unlock("m"),
    fence(),
};

const std::array<model::Action, 14> kBufferedFuzzActions{
    read("x"),
    read("y"),
    write("x"),
    write("y"),
    atomic_load("x"),
    atomic_load("y"),
    atomic_store("x"),
    atomic_store("y"),
    lock("m"),
    unlock("m"),
    fence(),
    yield(),
    set(0, 1),
    assert_nonzero(0),
};

model::Program two_thread_program(std::uint64_t encoded,
                                  std::size_t lhs_length,
                                  std::size_t rhs_length) {
    model::Program program;
    program.threads.resize(2);
    for (std::size_t i = 0; i < lhs_length; ++i) {
        program.threads[0].push_back(kTwoThreadActions.at(encoded % kTwoThreadActions.size()));
        encoded /= kTwoThreadActions.size();
    }
    for (std::size_t i = 0; i < rhs_length; ++i) {
        program.threads[1].push_back(kTwoThreadActions.at(encoded % kTwoThreadActions.size()));
        encoded /= kTwoThreadActions.size();
    }
    return program;
}

std::uint64_t pow_actions(std::size_t exponent) {
    std::uint64_t result = 1;
    for (std::size_t i = 0; i < exponent; ++i) {
        result *= kTwoThreadActions.size();
    }
    return result;
}

model::Program buffered_two_thread_program(std::uint64_t encoded,
                                           std::size_t lhs_length,
                                           std::size_t rhs_length) {
    model::Program program;
    program.threads.resize(2);
    for (std::size_t index = 0; index < lhs_length; ++index) {
        program.threads[0].push_back(kBufferedActions.at(encoded % kBufferedActions.size()));
        encoded /= kBufferedActions.size();
    }
    for (std::size_t index = 0; index < rhs_length; ++index) {
        program.threads[1].push_back(kBufferedActions.at(encoded % kBufferedActions.size()));
        encoded /= kBufferedActions.size();
    }
    return program;
}

std::uint64_t pow_buffered_actions(std::size_t exponent) {
    std::uint64_t result = 1;
    for (std::size_t index = 0; index < exponent; ++index) {
        result *= kBufferedActions.size();
    }
    return result;
}

std::vector<model::Program> hand_picked_meter_programs() {
    return {
        model::Program{{
            {yield(), yield(), yield()},
            {yield(), yield(), yield()},
        }},
        model::Program{{
            {write("x"), write("y")},
            {yield(), yield()},
        }},
        model::Program{{
            {lock("m"), write("x"), unlock("m")},
            {yield(), yield(), yield()},
        }},
        model::Program{{
            {yield(), yield(), lock("m"), unlock("m")},
            {yield(), yield(), lock("m"), unlock("m")},
        }},
        model::Program{{
            {write("a"), write("b"), lock("m"), unlock("m")},
            {write("c"), write("d"), lock("m"), unlock("m")},
        }},
        model::Program{{
            {join(1)},
            {yield(), yield(), yield()},
            {yield(), yield(), yield()},
            {yield(), yield(), yield()},
        }},
        model::Program{{
            {join(1)},
            {yield(), yield(), yield()},
            {join(1)},
            {yield(), yield(), yield()},
        }},
    };
}

std::size_t bounded(std::mt19937_64& rng, std::size_t limit) {
    assert(limit > 0);
    return std::uniform_int_distribution<std::size_t>(0, limit - 1)(rng);
}

model::ThreadId random_thread(std::mt19937_64& rng, std::size_t thread_count) {
    return static_cast<model::ThreadId>(bounded(rng, thread_count));
}

model::ThreadId random_other_thread(std::mt19937_64& rng,
                                    model::ThreadId self,
                                    std::size_t thread_count) {
    if (thread_count == 1) {
        return self;
    }
    model::ThreadId target = static_cast<model::ThreadId>(bounded(rng, thread_count - 1));
    if (target >= self) {
        ++target;
    }
    return target;
}

model::Action random_fuzz_action(std::mt19937_64& rng,
                                 model::ThreadId tid,
                                 std::size_t thread_count) {
    const std::size_t choice = bounded(rng, 18);
    switch (choice) {
    case 0:
        return read(bounded(rng, 2) == 0 ? "x" : "y");
    case 1:
    case 2:
        return write(bounded(rng, 2) == 0 ? "x" : "y");
    case 3:
        return atomic_load(bounded(rng, 2) == 0 ? "f" : "g");
    case 4:
        return atomic_store(bounded(rng, 2) == 0 ? "f" : "g");
    case 5:
        return atomic_rmw(bounded(rng, 2) == 0 ? "f" : "g");
    case 6:
        return cas(bounded(rng, 2) == 0 ? "f" : "g");
    case 7:
        return lock(bounded(rng, 2) == 0 ? "m" : "n");
    case 8:
        return unlock(bounded(rng, 2) == 0 ? "m" : "n");
    case 9:
        return wait(bounded(rng, 2) == 0 ? "cv0" : "cv1", bounded(rng, 2) == 0 ? "m" : "n");
    case 10:
        return signal(bounded(rng, 2) == 0 ? "cv0" : "cv1");
    case 11:
        return broadcast(bounded(rng, 2) == 0 ? "cv0" : "cv1");
    case 12:
        return spawn(bounded(rng, 4) == 0 ? tid : random_thread(rng, thread_count));
    case 13:
        return join(bounded(rng, 4) == 0 ? tid : random_other_thread(rng, tid, thread_count));
    case 14:
        return set(static_cast<model::RegisterId>(bounded(rng, model::kRegisterCount)),
                   static_cast<model::Value>(bounded(rng, 3)));
    case 15:
        return assert_nonzero(static_cast<model::RegisterId>(bounded(rng, model::kRegisterCount)));
    default:
        return yield();
    }
}

model::Program generate_fuzz_program(std::mt19937_64& rng) {
    const std::size_t thread_count = 2 + bounded(rng, 3);
    model::Program program;
    program.threads.resize(thread_count);
    for (std::size_t tid_index = 0; tid_index < thread_count; ++tid_index) {
        const auto tid = static_cast<model::ThreadId>(tid_index);
        const std::size_t action_count = 1 + bounded(rng, 4);
        for (std::size_t action_index = 0; action_index < action_count; ++action_index) {
            program.threads.at(tid).push_back(random_fuzz_action(rng, tid, thread_count));
        }
    }
    return program;
}

model::Program generate_buffered_fuzz_program(std::mt19937_64& rng) {
    model::Program program;
    program.threads.resize(2);
    for (auto& thread : program.threads) {
        const std::size_t action_count = 1 + bounded(rng, 3);
        for (std::size_t index = 0; index < action_count; ++index) {
            thread.push_back(kBufferedFuzzActions.at(bounded(rng, kBufferedFuzzActions.size())));
        }
    }
    return program;
}

std::string operand_key(const std::optional<model::ValueOperand>& operand) {
    if (!operand.has_value()) {
        return "-";
    }
    std::ostringstream out;
    out << static_cast<int>(operand->kind) << ':' << operand->immediate
        << ':' << static_cast<unsigned>(operand->reg);
    return out.str();
}

std::string optional_reg_key(const std::optional<model::RegisterId>& reg) {
    if (!reg.has_value()) {
        return "-";
    }
    return std::to_string(static_cast<unsigned>(*reg));
}

std::string action_key(const model::Action& action) {
    std::ostringstream out;
    out << static_cast<int>(action.kind)
        << "|a=" << action.address
        << "|m=" << action.mutex
        << "|rw=" << action.rwlock
        << "|sem=" << action.semaphore
        << "|bar=" << action.barrier
        << "|parties=" << action.parties
        << "|c=" << action.condition
        << "|t=" << action.target
        << "|d=" << optional_reg_key(action.destination)
        << "|s=" << optional_reg_key(action.source_register)
        << "|v=" << operand_key(action.value)
        << "|e=" << operand_key(action.expected)
        << "|l=" << action.label;
    return out.str();
}

std::string transition_label(const model::EffectiveScheduleStep& step, std::size_t occurrence) {
    std::ostringstream out;
    out << "t=" << step.endpoint.thread
        << "|pc=" << step.endpoint.action_index
        << "|occ=" << occurrence
        << "|" << action_key(step.effective_action);
    return out.str();
}

std::vector<std::string> transition_labels(const std::vector<model::EffectiveScheduleStep>& trace) {
    std::map<std::string, std::size_t> occurrences;
    std::vector<std::string> labels;
    labels.reserve(trace.size());
    for (const auto& step : trace) {
        std::ostringstream base;
        base << step.endpoint.thread << '|' << step.endpoint.action_index
             << '|' << action_key(step.effective_action);
        const std::size_t occurrence = occurrences[base.str()]++;
        labels.push_back(transition_label(step, occurrence));
    }
    return labels;
}

std::vector<std::string> canonical_form(const model::ModelChecker& checker,
                                        model::MemoryModel memory_model,
                                        const std::vector<model::EffectiveScheduleStep>& trace,
                                        ClassRelation relation = ClassRelation::Current) {
    const std::vector<std::string> labels = transition_labels(trace);
    std::vector<std::vector<std::size_t>> outgoing(trace.size());
    std::vector<std::size_t> indegree(trace.size(), 0);

    for (std::size_t i = 0; i < trace.size(); ++i) {
        for (std::size_t j = i + 1; j < trace.size(); ++j) {
            // This gate measures optimality against the same correspondence
            // DPOR may exploit. Program-ordered pairs and source/flush pairs
            // stay ordered. TSO flushes stay ordered by its single FIFO, while
            // PSO flushes from one thread are ordered only at the same address;
            // different-address drains may commute unless another dependence
            // path orders them. Enabled valid Join gets the checker-local ADR
            // 0011 refinement, and all other pairs fall back to action-level
            // independent(). A stricter relation would inflate the class count.
            bool transition_independent = checker.dpor_transitions_independent(
                trace[i].endpoint.thread,
                trace[i].effective_action,
                trace[j].endpoint.thread,
                trace[j].effective_action);
            if (relation == ClassRelation::OriginalBufferedWriteVisibility &&
                memory_model != model::MemoryModel::SC &&
                trace[i].endpoint.thread != trace[j].endpoint.thread &&
                (trace[i].effective_action.kind == model::ActionKind::Write ||
                 trace[j].effective_action.kind == model::ActionKind::Write) &&
                trace[i].effective_action.kind != model::ActionKind::Join &&
                trace[j].effective_action.kind != model::ActionKind::Join) {
                // Historical accounting only: before buffered source writes
                // were distinguished from globally visible writes, this pair
                // fell through to the action-level predicate. The resulting
                // stricter classes are not a sound lower bound for the widened
                // checker relation; retain them only to compare with ADR 0019.
                transition_independent = model::independent(
                    trace[i].effective_action, trace[j].effective_action);
            }
            const bool dependent =
                same_thread_steps_ordered(memory_model, trace[i], trace[j]) ||
                !transition_independent;
            if (!dependent) {
                continue;
            }
            outgoing[i].push_back(j);
            ++indegree[j];
        }
    }

    struct ByLabel {
        const std::vector<std::string>* labels{nullptr};

        bool operator()(std::size_t lhs, std::size_t rhs) const {
            if (labels->at(lhs) != labels->at(rhs)) {
                return labels->at(lhs) < labels->at(rhs);
            }
            return lhs < rhs;
        }
    };

    std::set<std::size_t, ByLabel> available((ByLabel{&labels}));
    for (std::size_t index = 0; index < indegree.size(); ++index) {
        if (indegree[index] == 0) {
            available.insert(index);
        }
    }

    std::vector<std::string> canonical;
    canonical.reserve(trace.size());
    while (!available.empty()) {
        const std::size_t selected = *available.begin();
        available.erase(available.begin());
        canonical.push_back(labels.at(selected));
        for (const std::size_t child : outgoing.at(selected)) {
            assert(indegree.at(child) > 0);
            --indegree.at(child);
            if (indegree.at(child) == 0) {
                available.insert(child);
            }
        }
    }

    assert(canonical.size() == trace.size());
    return canonical;
}

std::size_t class_count_for(const model::Program& program,
                            model::MemoryModel memory_model,
                            ClassRelation relation) {
    const model::ModelChecker checker(
        program, model::ModelChecker::kDefaultStepBound, memory_model);
    const model::CheckResult naive = checker.explore_naive(kNoScheduleCap);
    const std::vector<model::Schedule> schedules =
        checker.collect_naive_schedules(naive.schedules_explored + 1);
    require(schedules.size() == naive.schedules_explored,
            "class-count fixture did not collect every naive schedule");

    std::set<std::vector<std::string>> classes;
    for (const model::Schedule& schedule : schedules) {
        classes.insert(canonical_form(
            checker, memory_model, checker.replay_effective_trace(schedule), relation));
    }
    return classes.size();
}

VerdictKind verdict_kind(const model::CheckResult& result) {
    if (result.first_race.has_value()) {
        return VerdictKind::Race;
    }
    if (result.first_deadlock.has_value()) {
        return VerdictKind::Deadlock;
    }
    if (result.first_error.has_value()) {
        return VerdictKind::Error;
    }
    if (result.first_assertion.has_value()) {
        return VerdictKind::Assertion;
    }
    if (result.first_nontermination.has_value()) {
        return VerdictKind::NonTermination;
    }
    if (result.bound_exceeded_executions > 0) {
        return VerdictKind::Bound;
    }
    return VerdictKind::Clean;
}

const char* verdict_name(VerdictKind verdict) {
    switch (verdict) {
    case VerdictKind::Clean:
        return "clean";
    case VerdictKind::Race:
        return "race";
    case VerdictKind::Deadlock:
        return "deadlock";
    case VerdictKind::Error:
        return "error";
    case VerdictKind::Assertion:
        return "assertion";
    case VerdictKind::NonTermination:
        return "nontermination";
    case VerdictKind::Bound:
        return "bound";
    }
    return "unknown";
}

std::string action_string(const model::Action& action) {
    std::ostringstream out;
    switch (action.kind) {
    case model::ActionKind::Set:
        out << "Set";
        break;
    case model::ActionKind::Label:
        out << "Label " << action.label;
        break;
    case model::ActionKind::BranchNonzero:
        out << "BranchNonzero";
        break;
    case model::ActionKind::Assert:
        out << "Assert";
        break;
    case model::ActionKind::Read:
        out << "Read " << action.address;
        break;
    case model::ActionKind::Write:
        out << "Write " << action.address;
        break;
    case model::ActionKind::AtomicLoad:
        out << "AtomicLoad " << action.address;
        break;
    case model::ActionKind::AtomicStore:
        out << "AtomicStore " << action.address;
        break;
    case model::ActionKind::AtomicRmw:
        out << "AtomicRmw " << action.address;
        break;
    case model::ActionKind::CompareExchange:
        out << "CompareExchange " << action.address;
        break;
    case model::ActionKind::Fence:
        out << "Fence";
        break;
    case model::ActionKind::Flush:
        out << "Flush " << action.address;
        break;
    case model::ActionKind::Lock:
        out << "Lock " << action.mutex;
        break;
    case model::ActionKind::Unlock:
        out << "Unlock " << action.mutex;
        break;
    case model::ActionKind::Spawn:
        out << "Spawn " << action.target;
        break;
    case model::ActionKind::Join:
        out << "Join " << action.target;
        break;
    case model::ActionKind::Wait:
        out << "Wait " << action.condition << ' ' << action.mutex;
        break;
    case model::ActionKind::Signal:
        out << "Signal " << action.condition;
        break;
    case model::ActionKind::Broadcast:
        out << "Broadcast " << action.condition;
        break;
    case model::ActionKind::Yield:
        out << "Yield";
        break;
    case model::ActionKind::RLock:
        out << "RLock " << action.rwlock;
        break;
    case model::ActionKind::RUnlock:
        out << "RUnlock " << action.rwlock;
        break;
    case model::ActionKind::WLock:
        out << "WLock " << action.rwlock;
        break;
    case model::ActionKind::WUnlock:
        out << "WUnlock " << action.rwlock;
        break;
    case model::ActionKind::SemPost:
        out << "SemPost " << action.semaphore;
        break;
    case model::ActionKind::SemWait:
        out << "SemWait " << action.semaphore;
        break;
    case model::ActionKind::BarrierWait:
        out << "BarrierWait " << action.barrier << ' ' << action.parties;
        break;
    }
    return out.str();
}

void print_program(const model::Program& program) {
    for (std::size_t tid = 0; tid < program.threads.size(); ++tid) {
        std::cerr << "  t" << tid << ':';
        for (const auto& action : program.threads.at(tid)) {
            std::cerr << " [" << action_string(action) << ']';
        }
        std::cerr << '\n';
    }
}

bool eligible_for_meter(const model::CheckResult& naive, bool enforce_small_schedule_limit) {
    // Terminal modeled errors/assertions are still covered by the differential
    // gates. The optimality meter is scoped to clean/race/deadlock programs so
    // the measured class minimum stays comparable to DPOR's bug-existence
    // pruning of independent preludes before terminal reports.
    return !naive.exploration_capped &&
           naive.cycles_detected == 0 &&
           naive.bound_exceeded_executions == 0 &&
           !naive.first_error.has_value() &&
           !naive.first_assertion.has_value() &&
           (!enforce_small_schedule_limit ||
            naive.schedules_explored <= kMaxMeteredNaiveSchedules);
}

void add_source_count(AggregateStats& stats, ProgramSource source) {
    switch (source) {
    case ProgramSource::TwoThreadFamily:
        ++stats.two_thread.metered;
        break;
    case ProgramSource::HandPicked:
        ++stats.hand_picked.metered;
        break;
    case ProgramSource::Fuzz:
        ++stats.fuzz.metered;
        break;
    }
}

std::size_t measure_program(const model::Program& program,
                            ProgramSource source,
                            AggregateStats& stats,
                            bool enforce_small_schedule_limit,
                            model::MemoryModel memory_model = model::MemoryModel::SC) {
    const model::ModelChecker checker(
        program, model::ModelChecker::kDefaultStepBound, memory_model);
    // Buffered schedules gain internal flush transitions and can grow quickly.
    // A 25th leaf is sufficient to reject a candidate whose eligibility limit
    // is 24; only eligible spaces are then collected and replayed exhaustively.
    const std::size_t probe_cap =
        enforce_small_schedule_limit && memory_model != model::MemoryModel::SC
            ? kMaxMeteredNaiveSchedules + 1
            : kNoScheduleCap;
    const model::CheckResult naive = checker.explore_naive(probe_cap);
    if (!eligible_for_meter(naive, enforce_small_schedule_limit)) {
        return 0;
    }

    const model::CheckResult dpor = checker.explore_dpor(kNoScheduleCap);
    if (dpor.cycles_detected != 0 || dpor.bound_exceeded_executions != 0) {
        std::cerr << "DPOR left the zero-cycle/zero-bound meter scope\n";
        print_program(program);
        std::abort();
    }
    const std::vector<model::Schedule> schedules =
        checker.collect_naive_schedules(naive.schedules_explored + 1);
    if (schedules.size() != naive.schedules_explored) {
        std::cerr << "naive schedule collector mismatch: collected=" << schedules.size()
                  << " naive=" << naive.schedules_explored << '\n';
        print_program(program);
        std::abort();
    }

    std::map<std::vector<std::string>, VerdictKind> class_verdicts;
    std::set<std::vector<std::string>> original_relation_classes;
    for (const model::Schedule& schedule : schedules) {
        const std::vector<model::EffectiveScheduleStep> effective_trace =
            checker.replay_effective_trace(schedule);
        const std::vector<std::string> canonical =
            canonical_form(checker, memory_model, effective_trace);
        original_relation_classes.insert(canonical_form(
            checker,
            memory_model,
            effective_trace,
            ClassRelation::OriginalBufferedWriteVisibility));
        const VerdictKind verdict = verdict_kind(checker.replay(schedule));
        const auto [position, inserted] = class_verdicts.emplace(canonical, verdict);
        if (!inserted && position->second != verdict) {
            std::cerr << "SOUNDNESS BUG: one Mazurkiewicz class produced verdicts "
                      << verdict_name(position->second) << " and " << verdict_name(verdict)
                      << '\n';
            print_program(program);
            std::abort();
        }
    }

    const std::size_t class_count = class_verdicts.size();
    if (class_count > dpor.schedules_explored || dpor.schedules_explored > naive.schedules_explored) {
        std::cerr << "optimality inequality failed: classes=" << class_count
                  << " dpor=" << dpor.schedules_explored
                  << " naive=" << naive.schedules_explored << '\n';
        print_program(program);
        std::abort();
    }

    ++stats.programs_metered;
    add_source_count(stats, source);
    stats.total_classes += class_count;
    stats.total_original_relation_classes += original_relation_classes.size();
    stats.total_dpor_schedules += dpor.schedules_explored;
    stats.total_naive_schedules += naive.schedules_explored;
    if (dpor.schedules_explored == class_count) {
        ++stats.optimal_programs;
    }
    return 1;
}

void measure_two_thread_family(AggregateStats& stats) {
    constexpr std::uint64_t kProgramsPerLengthPairCap = 2048;
    for (std::size_t lhs_length = 0; lhs_length <= 3; ++lhs_length) {
        for (std::size_t rhs_length = 0; rhs_length <= 3; ++rhs_length) {
            const auto count = pow_actions(lhs_length + rhs_length);
            const auto samples = std::min<std::uint64_t>(count, kProgramsPerLengthPairCap);
            for (std::uint64_t sample = 0; sample < samples; ++sample) {
                const auto encoded = count == samples ? sample : (sample * count) / samples;
                measure_program(
                    two_thread_program(encoded, lhs_length, rhs_length),
                    ProgramSource::TwoThreadFamily,
                    stats,
                    true);
            }
        }
    }
}

void measure_hand_picked(AggregateStats& stats) {
    for (const model::Program& program : hand_picked_meter_programs()) {
        const std::size_t added = measure_program(program, ProgramSource::HandPicked, stats, false);
        if (added != 1) {
            std::cerr << "hand-picked optimality fixture was unexpectedly ineligible\n";
            print_program(program);
            std::abort();
        }
    }
}

void measure_fuzz_sample(AggregateStats& stats) {
    std::mt19937_64 rng(0x4712d907c0ffee21ull);
    constexpr std::size_t kFuzzCandidates = 256;
    for (std::size_t index = 0; index < kFuzzCandidates; ++index) {
        measure_program(generate_fuzz_program(rng), ProgramSource::Fuzz, stats, true);
    }
}

void measure_buffered_model(model::MemoryModel memory_model, AggregateStats& stats) {
    require(memory_model == model::MemoryModel::TSO ||
                memory_model == model::MemoryModel::PSO,
            "buffered optimality corpus requires TSO or PSO");

    constexpr std::uint64_t kProgramsPerLengthPairCap = 128;
    for (std::size_t lhs_length = 0; lhs_length <= 2; ++lhs_length) {
        for (std::size_t rhs_length = 0; rhs_length <= 2; ++rhs_length) {
            const std::uint64_t count = pow_buffered_actions(lhs_length + rhs_length);
            const std::uint64_t samples =
                std::min<std::uint64_t>(count, kProgramsPerLengthPairCap);
            for (std::uint64_t sample = 0; sample < samples; ++sample) {
                const std::uint64_t encoded =
                    count == samples ? sample : (sample * count) / samples;
                measure_program(
                    buffered_two_thread_program(encoded, lhs_length, rhs_length),
                    ProgramSource::TwoThreadFamily,
                    stats,
                    true,
                    memory_model);
            }
        }
    }

    std::mt19937_64 rng(0x7b83d52fa9614c0dull);
    constexpr std::size_t kFuzzCandidates = 128;
    for (std::size_t index = 0; index < kFuzzCandidates; ++index) {
        measure_program(
            generate_buffered_fuzz_program(rng),
            ProgramSource::Fuzz,
            stats,
            true,
            memory_model);
    }
}

void print_summary(const AggregateStats& stats) {
    const double redundancy_ratio =
        stats.total_classes == 0
            ? 0.0
            : static_cast<double>(stats.total_dpor_schedules) /
                  static_cast<double>(stats.total_classes);
    const double optimal_percent =
        stats.programs_metered == 0
            ? 0.0
            : 100.0 * static_cast<double>(stats.optimal_programs) /
                  static_cast<double>(stats.programs_metered);

    std::cout << "dpor_optimality: programs metered=" << stats.programs_metered
              << " total_classes=" << stats.total_classes
              << " total_dpor_schedules=" << stats.total_dpor_schedules
              << " total_naive_schedules=" << stats.total_naive_schedules
              << std::fixed << std::setprecision(3)
              << " redundancy_ratio=" << redundancy_ratio
              << std::setprecision(1)
              << " optimal_programs_percent=" << optimal_percent
              << " source_two_thread=" << stats.two_thread.metered
              << " source_hand_picked=" << stats.hand_picked.metered
              << " source_fuzz=" << stats.fuzz.metered
              << " within_class_same_verdict=held\n";
}

void print_buffered_summary(model::MemoryModel memory_model, const AggregateStats& stats) {
    const double redundancy_ratio =
        stats.total_classes == 0
            ? 0.0
            : static_cast<double>(stats.total_dpor_schedules) /
                  static_cast<double>(stats.total_classes);
    const double optimal_percent =
        stats.programs_metered == 0
            ? 0.0
            : 100.0 * static_cast<double>(stats.optimal_programs) /
                  static_cast<double>(stats.programs_metered);
    const double original_relation_ratio =
        stats.total_original_relation_classes == 0
            ? 0.0
            : static_cast<double>(stats.total_dpor_schedules) /
                  static_cast<double>(stats.total_original_relation_classes);
    const char* prefix = memory_model == model::MemoryModel::TSO
                             ? "dpor_optimality_tso:"
                             : "dpor_optimality_pso:";

    std::cout << prefix
              << " programs metered=" << stats.programs_metered
              << " total_classes=" << stats.total_classes
              << " total_dpor_schedules=" << stats.total_dpor_schedules
              << " total_naive_schedules=" << stats.total_naive_schedules
              << std::fixed << std::setprecision(3)
              << " redundancy_ratio=" << redundancy_ratio
              << " original_relation_classes=" << stats.total_original_relation_classes
              << " original_relation_ratio=" << original_relation_ratio
              << std::setprecision(1)
              << " optimal_programs_percent=" << optimal_percent
              << " source_enumerated=" << stats.two_thread.metered
              << " source_fuzz=" << stats.fuzz.metered
              << " within_class_same_verdict=held\n";
}

void print_refinement_representative(const char* name,
                                     const model::Program& program,
                                     model::MemoryModel memory_model) {
    const model::ModelChecker checker(
        program, model::ModelChecker::kDefaultStepBound, memory_model);
    const model::CheckResult naive = checker.explore_naive(kNoScheduleCap);
    const model::CheckResult dpor = checker.explore_dpor(kNoScheduleCap);
    std::cout << "dpor_refinement_representative: name=" << name
              << " model="
              << (memory_model == model::MemoryModel::TSO ? "TSO" : "PSO")
              << " current_classes=" << class_count_for(program, memory_model)
              << " original_relation_classes="
              << class_count_for(
                     program,
                     memory_model,
                     ClassRelation::OriginalBufferedWriteVisibility)
              << " dpor_schedules=" << dpor.schedules_explored
              << " naive_schedules=" << naive.schedules_explored << '\n';
}

} // namespace

int main() {
    test_same_thread_flush_correspondence();
    test_replay_exposes_pso_flush_identity();
    test_pso_flush_reordering_changes_class_count();

    const model::Program enqueue_read_repair{{
        {write("y")},
        {read("x"), read("y")},
    }};
    print_refinement_representative(
        "enqueue_read_repair", enqueue_read_repair, model::MemoryModel::TSO);
    print_refinement_representative(
        "enqueue_read_repair", enqueue_read_repair, model::MemoryModel::PSO);
    print_refinement_representative(
        "pso_flush_siblings",
        model::Program{{
            {write("y"), write("x")},
            {atomic_load("x")},
        }},
        model::MemoryModel::PSO);

    AggregateStats stats;
    measure_two_thread_family(stats);
    measure_hand_picked(stats);
    measure_fuzz_sample(stats);
    print_summary(stats);

    AggregateStats tso_stats;
    measure_buffered_model(model::MemoryModel::TSO, tso_stats);
    require(tso_stats.two_thread.metered > 0,
            "TSO optimality corpus has no eligible oracle-family programs");
    require(tso_stats.fuzz.metered > 0,
            "TSO optimality corpus has no eligible fixed-seed fuzz programs");
    print_buffered_summary(model::MemoryModel::TSO, tso_stats);

    AggregateStats pso_stats;
    measure_buffered_model(model::MemoryModel::PSO, pso_stats);
    require(pso_stats.two_thread.metered > 0,
            "PSO optimality corpus has no eligible oracle-family programs");
    require(pso_stats.fuzz.metered > 0,
            "PSO optimality corpus has no eligible fixed-seed fuzz programs");
    print_buffered_summary(model::MemoryModel::PSO, pso_stats);
    return 0;
}
