#include "json_writer.hpp"

#include "model/checker.hpp"
#include "program_parser.hpp"
#include "report.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using Json = showcase::json::Value;

struct Options {
    std::string program_path;
    std::string program_id;
    model::MemoryModel memory_model{model::MemoryModel::SC};
    std::size_t step_bound{model::ModelChecker::kDefaultStepBound};
    std::size_t max_schedules{100000};
};

struct TimedResult {
    model::CheckResult result;
    std::uint64_t wall_clock_us{0};
};

struct PrefixOrder {
    bool operator()(const model::Schedule& lhs, const model::Schedule& rhs) const {
        if (lhs.size() != rhs.size()) {
            return lhs.size() < rhs.size();
        }
        return lhs < rhs;
    }
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

std::size_t parse_size(const std::string& text, const char* description) {
    if (text.empty()) {
        fail(std::string("missing ") + description);
    }
    std::size_t value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            fail(std::string("invalid ") + description);
        }
        const std::size_t digit = static_cast<std::size_t>(character - '0');
        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
            fail(std::string(description) + " is too large");
        }
        value = value * 10 + digit;
    }
    if (value == 0) {
        fail(std::string(description) + " must be greater than zero");
    }
    return value;
}

model::MemoryModel parse_memory_model(const std::string& text) {
    if (text == "sc") {
        return model::MemoryModel::SC;
    }
    if (text == "tso") {
        return model::MemoryModel::TSO;
    }
    if (text == "pso") {
        return model::MemoryModel::PSO;
    }
    fail("invalid memory model '" + text + "'");
}

const char* memory_model_text(model::MemoryModel memory_model) {
    switch (memory_model) {
    case model::MemoryModel::SC: return "sc";
    case model::MemoryModel::TSO: return "tso";
    case model::MemoryModel::PSO: return "pso";
    }
    fail("unknown memory model");
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; index += 2) {
        if (index + 1 >= argc) {
            fail("every exporter option requires a value");
        }
        const std::string flag = argv[index];
        const std::string value = argv[index + 1];
        if (flag == "--program") {
            options.program_path = value;
        } else if (flag == "--program-id") {
            options.program_id = value;
        } else if (flag == "--memory-model") {
            options.memory_model = parse_memory_model(value);
        } else if (flag == "--step-bound") {
            options.step_bound = parse_size(value, "step bound");
        } else if (flag == "--max-schedules") {
            options.max_schedules = parse_size(value, "max schedules");
        } else {
            fail("unknown exporter option '" + flag + "'");
        }
    }
    require(!options.program_path.empty(), "--program is required");
    require(!options.program_id.empty(), "--program-id is required");
    return options;
}

const char* action_kind_text(model::ActionKind kind) {
    switch (kind) {
    case model::ActionKind::Set: return "set";
    case model::ActionKind::Label: return "label";
    case model::ActionKind::BranchNonzero: return "branch_nonzero";
    case model::ActionKind::Assert: return "assert";
    case model::ActionKind::Read: return "read";
    case model::ActionKind::Write: return "write";
    case model::ActionKind::AtomicLoad: return "atomic_load";
    case model::ActionKind::AtomicStore: return "atomic_store";
    case model::ActionKind::AtomicRmw: return "atomic_rmw";
    case model::ActionKind::CompareExchange: return "compare_exchange";
    case model::ActionKind::Fence: return "fence";
    case model::ActionKind::Flush: return "flush";
    case model::ActionKind::Lock: return "lock";
    case model::ActionKind::TryLock: return "try_lock";
    case model::ActionKind::Unlock: return "unlock";
    case model::ActionKind::Spawn: return "spawn";
    case model::ActionKind::Join: return "join";
    case model::ActionKind::Wait: return "wait";
    case model::ActionKind::Signal: return "signal";
    case model::ActionKind::Broadcast: return "broadcast";
    case model::ActionKind::Yield: return "yield";
    case model::ActionKind::RLock: return "rlock";
    case model::ActionKind::RUnlock: return "runlock";
    case model::ActionKind::WLock: return "wlock";
    case model::ActionKind::WUnlock: return "wunlock";
    case model::ActionKind::SemPost: return "sem_post";
    case model::ActionKind::SemWait: return "sem_wait";
    case model::ActionKind::BarrierWait: return "barrier_wait";
    case model::ActionKind::Upgrade: return "upgrade";
    case model::ActionKind::Downgrade: return "downgrade";
    case model::ActionKind::TimedWait: return "timed_wait";
    }
    fail("unknown action kind");
}

Json schedule_step_json(const model::ScheduleStep& step) {
    Json json = Json::object();
    json["action_index"] = step.action_index;
    if (step.flush_address.has_value()) {
        json["flush_address_id"] = *step.flush_address;
    } else {
        json["flush_address_id"] = nullptr;
    }
    json["thread"] = step.thread;
    return json;
}

Json schedule_json(const model::Schedule& schedule) {
    Json json = Json::array();
    for (const model::ScheduleStep& step : schedule) {
        json.push(schedule_step_json(step));
    }
    return json;
}

Json operand_json(const std::optional<model::ValueOperand>& operand) {
    if (!operand.has_value()) {
        return nullptr;
    }
    Json json = Json::object();
    if (operand->kind == model::ValueOperandKind::Register) {
        json["kind"] = "register";
        json["register"] = operand->reg;
    } else {
        json["immediate"] = operand->immediate;
        json["kind"] = "immediate";
    }
    return json;
}

Json action_json(const model::Action& action) {
    Json json = Json::object();
    json["address"] = action.address.empty() ? Json(nullptr) : Json(action.address);
    json["barrier"] = action.barrier.empty() ? Json(nullptr) : Json(action.barrier);
    json["condition"] = action.condition.empty() ? Json(nullptr) : Json(action.condition);
    if (action.destination.has_value()) {
        json["destination_register"] = *action.destination;
    } else {
        json["destination_register"] = nullptr;
    }
    json["expected"] = operand_json(action.expected);
    json["kind"] = action_kind_text(action.kind);
    json["label"] = cli::action_text(action);
    json["mutex"] = action.mutex.empty() ? Json(nullptr) : Json(action.mutex);
    json["parties"] = action.parties;
    json["rwlock"] = action.rwlock.empty() ? Json(nullptr) : Json(action.rwlock);
    json["semaphore"] = action.semaphore.empty() ? Json(nullptr) : Json(action.semaphore);
    if (action.source_register.has_value()) {
        json["source_register"] = *action.source_register;
    } else {
        json["source_register"] = nullptr;
    }
    json["target_thread"] = action.target;
    json["value"] = operand_json(action.value);
    return json;
}

Json clock_json(const std::vector<std::uint64_t>& clock) {
    Json json = Json::array();
    for (const std::uint64_t value : clock) {
        json.push(value);
    }
    return json;
}

bool clock_leq(const std::vector<std::uint64_t>& lhs,
               const std::vector<std::uint64_t>& rhs) {
    require(lhs.size() == rhs.size(), "vector-clock widths differ");
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        if (lhs.at(index) > rhs.at(index)) {
            return false;
        }
    }
    return true;
}

Json inspected_step_json(const model::InspectedScheduleStep& step,
                         std::size_t index,
                         const std::vector<model::InspectedScheduleStep>& trace) {
    Json json = Json::object();
    json["action"] = action_json(step.effective_action);
    json["clock_after"] = clock_json(step.clock_after);
    json["clock_before"] = clock_json(step.clock_before);
    json["endpoint"] = schedule_step_json(step.endpoint);
    json["executed"] = step.executed;

    Json enabled = Json::array();
    for (const model::ScheduleStep& endpoint : step.enabled_before) {
        enabled.push(schedule_step_json(endpoint));
    }
    json["enabled_before"] = std::move(enabled);

    Json hb = Json::array();
    if (step.executed) {
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (trace.at(prior).executed &&
                clock_leq(trace.at(prior).clock_after, step.clock_after)) {
                hb.push(prior);
            }
        }
    }
    json["happens_before_predecessors"] = std::move(hb);
    json["index"] = index;

    Json register_effects = Json::array();
    for (const model::RegisterEffect& effect : step.register_effects) {
        Json effect_json = Json::object();
        effect_json["after"] = effect.after;
        effect_json["before"] = effect.before;
        effect_json["register"] = effect.reg;
        effect_json["thread"] = effect.thread;
        register_effects.push(std::move(effect_json));
    }
    json["register_effects"] = std::move(register_effects);

    Json memory_effects = Json::array();
    for (const model::MemoryEffect& effect : step.memory_effects) {
        Json effect_json = Json::object();
        effect_json["address"] = effect.address;
        effect_json["after"] = effect.after.has_value() ? Json(*effect.after) : Json(nullptr);
        effect_json["before"] = effect.before.has_value() ? Json(*effect.before) : Json(nullptr);
        memory_effects.push(std::move(effect_json));
    }
    json["memory_effects"] = std::move(memory_effects);

    Json buffer_effects = Json::array();
    for (const model::BufferEffect& effect : step.buffer_effects) {
        Json effect_json = Json::object();
        const auto stores_json = [](const std::vector<model::BufferedStoreObservation>& stores) {
            Json values = Json::array();
            for (const model::BufferedStoreObservation& store : stores) {
                Json store_json = Json::object();
                store_json["address"] = store.address;
                store_json["value"] = store.value;
                values.push(std::move(store_json));
            }
            return values;
        };
        effect_json["after"] = stores_json(effect.after);
        effect_json["before"] = stores_json(effect.before);
        effect_json["thread"] = effect.thread;
        buffer_effects.push(std::move(effect_json));
    }
    json["store_buffer_effects"] = std::move(buffer_effects);

    if (step.broadcast_waking_set.has_value()) {
        Json wake = Json::array();
        for (const model::ThreadId tid : *step.broadcast_waking_set) {
            wake.push(tid);
        }
        json["broadcast_waking_set"] = std::move(wake);
    } else {
        json["broadcast_waking_set"] = nullptr;
    }

    if (step.timed_wait_occurrence.has_value()) {
        Json occurrence = Json::object();
        occurrence["episode"] = step.timed_wait_occurrence->episode;
        occurrence["transition"] =
            step.timed_wait_occurrence->transition == model::TimedWaitTransition::Park
                ? "park"
                : "timeout";
        json["timed_wait_occurrence"] = std::move(occurrence);
    } else {
        json["timed_wait_occurrence"] = nullptr;
    }
    return json;
}

Json detailed_trace_json(const std::vector<model::InspectedScheduleStep>& trace) {
    Json json = Json::array();
    for (std::size_t index = 0; index < trace.size(); ++index) {
        json.push(inspected_step_json(trace.at(index), index, trace));
    }
    return json;
}

Json bug_shape_json(const model::CheckResult& result) {
    Json json = Json::object();
    json["assertion"] = result.first_assertion.has_value();
    json["deadlock"] = result.first_deadlock.has_value();
    json["error"] = result.first_error.has_value();
    json["nontermination"] = result.first_nontermination.has_value();
    json["race"] = result.first_race.has_value();
    return json;
}

Json result_summary_json(const model::CheckResult& result) {
    Json json = Json::object();
    json["bound_exceeded_executions"] = result.bound_exceeded_executions;
    json["bug_shape"] = bug_shape_json(result);
    json["cycles_detected"] = result.cycles_detected;
    json["exploration_capped"] = result.exploration_capped;
    json["fair_cycles"] = result.fair_cycles;
    json["primary_verdict"] = cli::verdict_of(result);
    json["schedules_explored"] = result.schedules_explored;
    json["strongly_unfair_cycles"] = result.strongly_unfair_cycles;
    json["unfair_cycles"] = result.unfair_cycles;
    return json;
}

bool same_bug_existence(const model::CheckResult& lhs,
                        const model::CheckResult& rhs) {
    return lhs.first_race.has_value() == rhs.first_race.has_value() &&
           lhs.first_deadlock.has_value() == rhs.first_deadlock.has_value() &&
           lhs.first_error.has_value() == rhs.first_error.has_value() &&
           lhs.first_assertion.has_value() == rhs.first_assertion.has_value() &&
           lhs.first_nontermination.has_value() == rhs.first_nontermination.has_value();
}

template <typename Operation>
TimedResult timed(Operation operation) {
    const auto start = std::chrono::steady_clock::now();
    model::CheckResult result = operation();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return TimedResult{
        std::move(result),
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()),
    };
}

std::size_t common_prefix_length(const model::Schedule& lhs,
                                 const model::Schedule& rhs) {
    std::size_t length = 0;
    while (length < lhs.size() && length < rhs.size() &&
           lhs.at(length) == rhs.at(length)) {
        ++length;
    }
    return length;
}

std::size_t prefix_state_count(const std::vector<model::Schedule>& schedules) {
    if (schedules.empty()) {
        return 1;
    }
    std::vector<const model::Schedule*> ordered;
    ordered.reserve(schedules.size());
    for (const model::Schedule& schedule : schedules) {
        ordered.push_back(&schedule);
    }
    std::sort(
        ordered.begin(),
        ordered.end(),
        [](const model::Schedule* lhs, const model::Schedule* rhs) {
            return *lhs < *rhs;
        });
    std::size_t states = 1;
    model::Schedule previous;
    for (const model::Schedule* schedule_ptr : ordered) {
        const model::Schedule& schedule = *schedule_ptr;
        states += schedule.size() - common_prefix_length(previous, schedule);
        previous = schedule;
    }
    return states;
}

std::size_t max_depth(const std::vector<model::Schedule>& schedules) {
    std::size_t depth = 0;
    for (const model::Schedule& schedule : schedules) {
        depth = std::max(depth, schedule.size());
    }
    return depth;
}

std::vector<std::uint64_t> clock_for_endpoint(
    const std::vector<model::InspectedScheduleStep>& trace,
    const model::ScheduleStep& endpoint) {
    std::optional<std::vector<std::uint64_t>> clock;
    for (const model::InspectedScheduleStep& step : trace) {
        if (step.endpoint == endpoint && step.executed) {
            clock = step.clock_after;
        }
    }
    require(clock.has_value(), "race endpoint missing from detailed trace");
    return *clock;
}

Json blocked_thread_json(const model::BlockedThread& blocked) {
    Json json = Json::object();
    json["barrier"] = blocked.barrier.empty() ? Json(nullptr) : Json(blocked.barrier);
    json["condition"] = blocked.condition.empty() ? Json(nullptr) : Json(blocked.condition);
    const char* kind = "mutex";
    switch (blocked.kind) {
    case model::BlockedOnKind::Mutex: kind = "mutex"; break;
    case model::BlockedOnKind::Thread: kind = "thread"; break;
    case model::BlockedOnKind::ConditionVariable: kind = "condition_variable"; break;
    case model::BlockedOnKind::RwLockWriter: kind = "rwlock_writer"; break;
    case model::BlockedOnKind::RwLockReaders: kind = "rwlock_readers"; break;
    case model::BlockedOnKind::Semaphore: kind = "semaphore"; break;
    case model::BlockedOnKind::Barrier: kind = "barrier"; break;
    case model::BlockedOnKind::RwLockUpgrade: kind = "rwlock_upgrade"; break;
    }
    json["kind"] = kind;
    json["mutex"] = blocked.mutex.empty() ? Json(nullptr) : Json(blocked.mutex);
    json["owner"] = blocked.owner.has_value() ? Json(*blocked.owner) : Json(nullptr);
    json["rwlock"] = blocked.rwlock.empty() ? Json(nullptr) : Json(blocked.rwlock);
    json["self_wait"] = blocked.self_wait;
    json["semaphore"] = blocked.semaphore.empty() ? Json(nullptr) : Json(blocked.semaphore);
    json["target_thread"] = blocked.target.has_value() ? Json(*blocked.target) : Json(nullptr);
    json["thread"] = blocked.thread;
    return json;
}

const char* fairness_text(model::Fairness fairness) {
    switch (fairness) {
    case model::Fairness::UnfairScheduleWitness: return "unfair-schedule witness";
    case model::Fairness::StronglyUnfairScheduleWitness:
        return "strongly-unfair-schedule witness";
    case model::Fairness::FairDivergence: return "fair divergence";
    }
    fail("unknown fairness");
}

Json replay_primary_report_json(const model::CheckResult& replayed,
                                const model::ModelChecker& checker) {
    Json report = Json::object();
    model::Schedule schedule;
    if (replayed.first_race.has_value()) {
        const model::RaceReport& race = *replayed.first_race;
        schedule = race.schedule;
        const auto trace = checker.inspect_schedule(schedule);
        Json conflict = Json::object();
        conflict["address"] = race.address;
        conflict["first"] = schedule_step_json(race.first);
        conflict["first_clock"] = clock_json(clock_for_endpoint(trace, race.first));
        conflict["second"] = schedule_step_json(race.second);
        conflict["second_clock"] = clock_json(clock_for_endpoint(trace, race.second));
        report["conflict"] = std::move(conflict);
        report["kind"] = "race";
    } else if (replayed.first_deadlock.has_value()) {
        const model::DeadlockReport& deadlock = *replayed.first_deadlock;
        schedule = deadlock.schedule;
        Json blockers = Json::array();
        for (const model::BlockedThread& blocked : deadlock.blocked_threads) {
            blockers.push(blocked_thread_json(blocked));
        }
        report["blocked_threads"] = std::move(blockers);
        report["kind"] = "deadlock";
    } else if (replayed.first_error.has_value()) {
        const model::ModelErrorReport& error = *replayed.first_error;
        schedule = error.schedule;
        report["endpoint"] = schedule_step_json(error.endpoint);
        report["kind"] = "error";
        report["message"] = error.message;
    } else if (replayed.first_assertion.has_value()) {
        const model::AssertionFailureReport& assertion = *replayed.first_assertion;
        schedule = assertion.schedule;
        report["endpoint"] = schedule_step_json(assertion.endpoint);
        report["kind"] = "assertion";
        report["register"] = assertion.reg;
        report["value"] = assertion.value;
    } else if (replayed.first_nontermination.has_value()) {
        const model::NonTerminationReport& nontermination =
            *replayed.first_nontermination;
        schedule = nontermination.schedule;
        report["cycle"] = schedule_json(nontermination.cycle);
        report["fairness"] = fairness_text(nontermination.fairness);
        report["kind"] = "nontermination";
        report["stem"] = schedule_json(nontermination.stem);
    } else {
        fail("witness replay produced no primary bug report");
    }
    const auto trace = checker.inspect_schedule(schedule);
    report["schedule"] = schedule_json(schedule);
    report["steps"] = detailed_trace_json(trace);
    return report;
}

Json base_witness_json(const char* kind,
                       const model::Schedule& schedule,
                       const model::ModelChecker& checker) {
    const model::CheckResult replayed = checker.replay(schedule);
    const std::vector<model::InspectedScheduleStep> trace =
        checker.inspect_schedule(schedule);
    Json json = Json::object();
    json["kind"] = kind;
    json["reported_primary"] = replay_primary_report_json(replayed, checker);
    json["replay_outcome"] = result_summary_json(replayed);
    json["schedule"] = schedule_json(schedule);
    json["steps"] = detailed_trace_json(trace);
    return json;
}

Json evidence_json(const model::CheckResult& result,
                   const model::ModelChecker& checker) {
    Json evidence = Json::object();
    evidence["explorer"] = "dpor";
    Json property = Json::object();
    property["name"] = "assertion_reachability";
    property["reachable"] = result.first_assertion.has_value();
    property["verdict"] = result.first_assertion.has_value() ? "violated" : "holds";
    evidence["property"] = std::move(property);
    evidence["run_outcome"] = result_summary_json(result);

    Json witnesses = Json::array();
    if (result.first_race.has_value()) {
        const model::RaceReport& report = *result.first_race;
        require(checker.replay(report.schedule).first_race == result.first_race,
                "race witness did not replay identically");
        const auto trace = checker.inspect_schedule(report.schedule);
        Json witness = base_witness_json("race", report.schedule, checker);
        Json conflict = Json::object();
        conflict["address"] = report.address;
        conflict["first"] = schedule_step_json(report.first);
        conflict["first_clock"] = clock_json(clock_for_endpoint(trace, report.first));
        conflict["second"] = schedule_step_json(report.second);
        conflict["second_clock"] = clock_json(clock_for_endpoint(trace, report.second));
        witness["conflict"] = std::move(conflict);
        witnesses.push(std::move(witness));
    }
    if (result.first_deadlock.has_value()) {
        const model::DeadlockReport& report = *result.first_deadlock;
        require(checker.replay(report.schedule).first_deadlock == result.first_deadlock,
                "deadlock witness did not replay identically");
        Json witness = base_witness_json("deadlock", report.schedule, checker);
        Json blockers = Json::array();
        for (const model::BlockedThread& blocked : report.blocked_threads) {
            blockers.push(blocked_thread_json(blocked));
        }
        witness["blocked_threads"] = std::move(blockers);
        witnesses.push(std::move(witness));
    }
    if (result.first_error.has_value()) {
        const model::ModelErrorReport& report = *result.first_error;
        require(checker.replay(report.schedule).first_error == result.first_error,
                "modeled-error witness did not replay identically");
        Json witness = base_witness_json("error", report.schedule, checker);
        witness["endpoint"] = schedule_step_json(report.endpoint);
        witness["message"] = report.message;
        witnesses.push(std::move(witness));
    }
    if (result.first_assertion.has_value()) {
        const model::AssertionFailureReport& report = *result.first_assertion;
        require(checker.replay(report.schedule).first_assertion == result.first_assertion,
                "assertion witness did not replay identically");
        Json witness = base_witness_json("assertion", report.schedule, checker);
        witness["endpoint"] = schedule_step_json(report.endpoint);
        witness["register"] = report.reg;
        witness["value"] = report.value;
        witnesses.push(std::move(witness));
    }
    if (result.first_nontermination.has_value()) {
        const model::NonTerminationReport& report = *result.first_nontermination;
        require(checker.replay(report.schedule).first_nontermination ==
                    result.first_nontermination,
                "nontermination witness did not replay identically");
        Json witness = base_witness_json("nontermination", report.schedule, checker);
        witness["cycle"] = schedule_json(report.cycle);
        witness["fairness"] = fairness_text(report.fairness);
        witness["stem"] = schedule_json(report.stem);
        witnesses.push(std::move(witness));
    }
    evidence["witnesses"] = std::move(witnesses);
    return evidence;
}

Json run_json(const TimedResult& timed_result,
              const std::vector<model::Schedule>& schedules,
              std::size_t schedules_pruned) {
    Json json = result_summary_json(timed_result.result);
    json["max_depth"] = max_depth(schedules);
    json["prefix_states_visited"] = prefix_state_count(schedules);
    json["schedules_pruned"] = schedules_pruned;
    json["wall_clock_us"] = timed_result.wall_clock_us;
    json["wall_clock_note"] =
        "environment-dependent; excluded from deterministic comparison";
    return json;
}

Json tree_json(const model::ModelChecker& checker,
               const std::vector<model::Schedule>& naive_schedules,
               const std::vector<model::Schedule>& dpor_schedules,
               std::size_t expected_pruned) {
    std::set<model::Schedule> expanded;
    std::set<model::Schedule> terminals;
    std::map<model::Schedule, std::vector<model::ScheduleStep>> enabled_at;
    std::map<model::Schedule, model::InspectedScheduleStep> edge_observation;
    expanded.insert(model::Schedule{});

    for (const model::Schedule& schedule : dpor_schedules) {
        const auto trace = checker.inspect_schedule(schedule);
        require(trace.size() == schedule.size(),
                "inspection omitted a collected DPOR terminal step");
        model::Schedule prefix;
        for (std::size_t index = 0; index < schedule.size(); ++index) {
            const auto [enabled, inserted] =
                enabled_at.emplace(prefix, trace.at(index).enabled_before);
            if (!inserted) {
                require(enabled->second == trace.at(index).enabled_before,
                        "same numeric prefix produced different enabled sets");
            }
            prefix.push_back(schedule.at(index));
            expanded.insert(prefix);
            const auto [edge, edge_inserted] =
                edge_observation.emplace(prefix, trace.at(index));
            if (!edge_inserted) {
                require(edge->second == trace.at(index),
                        "same numeric prefix produced different transition evidence");
            }
        }
        terminals.insert(schedule);
    }

    std::set<model::Schedule> pruned_boundaries;
    for (const model::Schedule& prefix : expanded) {
        if (terminals.find(prefix) != terminals.end()) {
            continue;
        }
        const auto enabled = enabled_at.find(prefix);
        require(enabled != enabled_at.end(),
                "nonterminal expanded prefix omitted enabled transitions");
        for (const model::ScheduleStep& step : enabled->second) {
            model::Schedule child = prefix;
            child.push_back(step);
            if (expanded.find(child) != expanded.end()) {
                continue;
            }
            pruned_boundaries.insert(child);
            const auto inspected = checker.inspect_schedule(child);
            require(inspected.size() == child.size(),
                    "pruned boundary inspection omitted its edge");
            edge_observation.emplace(child, inspected.back());
        }
    }

    std::map<model::Schedule, std::size_t> naive_counts;
    std::map<model::Schedule, std::size_t> dpor_counts;
    for (const model::Schedule& prefix : expanded) {
        naive_counts.emplace(prefix, 0);
        dpor_counts.emplace(prefix, 0);
    }
    for (const model::Schedule& prefix : pruned_boundaries) {
        naive_counts.emplace(prefix, 0);
        dpor_counts.emplace(prefix, 0);
    }

    const auto count_prefixes = [](const std::vector<model::Schedule>& schedules,
                                   std::map<model::Schedule, std::size_t>& counts) {
        for (const model::Schedule& schedule : schedules) {
            model::Schedule prefix;
            if (auto root = counts.find(prefix); root != counts.end()) {
                ++root->second;
            }
            for (const model::ScheduleStep& step : schedule) {
                prefix.push_back(step);
                if (auto found = counts.find(prefix); found != counts.end()) {
                    ++found->second;
                }
            }
        }
    };
    count_prefixes(naive_schedules, naive_counts);
    count_prefixes(dpor_schedules, dpor_counts);

    std::size_t pruned_total = 0;
    for (const model::Schedule& prefix : pruned_boundaries) {
        require(naive_counts.at(prefix) > 0,
                "DPOR pruned a boundary absent from the naive tree");
        require(dpor_counts.at(prefix) == 0,
                "pruned boundary contains a DPOR representative");
        pruned_total += naive_counts.at(prefix);
    }
    require(pruned_total == expected_pruned,
            "pruned subtree counts do not conserve naive schedules");

    std::vector<model::Schedule> ordered_nodes(expanded.begin(), expanded.end());
    ordered_nodes.insert(
        ordered_nodes.end(), pruned_boundaries.begin(), pruned_boundaries.end());
    std::sort(ordered_nodes.begin(), ordered_nodes.end(), PrefixOrder{});
    std::map<model::Schedule, std::size_t> node_id;
    for (std::size_t index = 0; index < ordered_nodes.size(); ++index) {
        node_id.emplace(ordered_nodes.at(index), index);
    }

    Json nodes = Json::array();
    for (std::size_t index = 0; index < ordered_nodes.size(); ++index) {
        const model::Schedule& prefix = ordered_nodes.at(index);
        const bool pruned = pruned_boundaries.find(prefix) != pruned_boundaries.end();
        const bool terminal = terminals.find(prefix) != terminals.end();
        Json node = Json::object();
        node["choice_point"] = !pruned && !terminal && enabled_at.at(prefix).size() > 1;
        node["depth"] = prefix.size();
        Json enabled = Json::array();
        Json enabled_threads = Json::array();
        if (!pruned && !terminal) {
            std::set<model::ThreadId> threads;
            for (const model::ScheduleStep& endpoint : enabled_at.at(prefix)) {
                enabled.push(schedule_step_json(endpoint));
                threads.insert(endpoint.thread);
            }
            for (const model::ThreadId thread : threads) {
                enabled_threads.push(thread);
            }
        }
        node["dpor_schedules_below"] = dpor_counts.at(prefix);
        node["enabled"] = std::move(enabled);
        node["enabled_threads"] = std::move(enabled_threads);
        node["id"] = index;
        node["naive_schedules_below"] = naive_counts.at(prefix);
        node["status"] = pruned ? "PRUNED" : "EXPLORED";
        node["terminal"] = terminal;
        if (terminal) {
            node["outcome"] = result_summary_json(checker.replay(prefix));
        } else {
            node["outcome"] = nullptr;
        }
        nodes.push(std::move(node));
    }

    Json edges = Json::array();
    for (std::size_t target = 1; target < ordered_nodes.size(); ++target) {
        const model::Schedule& child = ordered_nodes.at(target);
        model::Schedule parent = child;
        const model::ScheduleStep step = parent.back();
        parent.pop_back();
        const auto source = node_id.find(parent);
        if (source == node_id.end()) {
            std::string diagnostic = "tree edge parent is absent: child_depth=" +
                std::to_string(child.size()) + " parent_depth=" +
                std::to_string(parent.size()) + " child=";
            for (const model::ScheduleStep& endpoint : child) {
                diagnostic += " (" + std::to_string(endpoint.thread) + "," +
                    std::to_string(endpoint.action_index) + ")";
            }
            fail(diagnostic);
        }
        const auto observed = edge_observation.find(child);
        require(observed != edge_observation.end(), "tree edge lacks action evidence");
        Json edge = Json::object();
        edge["action"] = action_json(observed->second.effective_action);
        edge["executed"] = observed->second.executed;
        edge["source"] = source->second;
        edge["status"] = pruned_boundaries.find(child) != pruned_boundaries.end()
            ? "PRUNED"
            : "EXPLORED";
        edge["step"] = schedule_step_json(step);
        edge["target"] = target;
        edges.push(std::move(edge));
    }

    Json tree = Json::object();
    tree["edges"] = std::move(edges);
    tree["expansion"] =
        "DPOR-reached prefixes plus exact counted boundaries for naive-only PRUNED subtrees";
    tree["nodes"] = std::move(nodes);
    tree["root"] = 0;
    return tree;
}

Json export_run(const Options& options) {
    const model::Program program = cli::parse_program_file(options.program_path);
    const model::ModelChecker checker(
        program, options.step_bound, options.memory_model);

    const TimedResult naive = timed([&] {
        return checker.explore_naive(options.max_schedules);
    });
    const TimedResult dpor = timed([&] {
        return checker.explore_dpor(options.max_schedules);
    });
    require(!naive.result.exploration_capped, "naive exploration reached its schedule cap");
    require(!dpor.result.exploration_capped, "DPOR exploration reached its schedule cap");
    require(same_bug_existence(naive.result, dpor.result),
            "naive and DPOR bug existence differs");

    const std::vector<model::Schedule> naive_schedules =
        checker.collect_naive_schedules(options.max_schedules);
    const std::vector<model::Schedule> dpor_schedules =
        checker.collect_dpor_schedules(options.max_schedules);
    require(naive_schedules.size() == naive.result.schedules_explored,
            "naive terminal collector count differs from exploration");
    require(dpor_schedules.size() == dpor.result.schedules_explored,
            "DPOR terminal collector count differs from exploration");
    require(dpor_schedules.size() <= naive_schedules.size(),
            "DPOR explored more schedules than naive");
    for (const model::Schedule& schedule : dpor_schedules) {
        require(std::binary_search(
                    naive_schedules.begin(), naive_schedules.end(), schedule),
                "DPOR terminal schedule is absent from naive exploration");
    }

    const std::size_t pruned = naive_schedules.size() - dpor_schedules.size();
    Json root = Json::object();
    Json configuration = Json::object();
    configuration["max_schedules"] = options.max_schedules;
    configuration["step_bound_per_thread"] = options.step_bound;
    root["configuration"] = std::move(configuration);
    root["evidence"] = evidence_json(dpor.result, checker);
    root["memory_model"] = memory_model_text(options.memory_model);
    root["program_id"] = options.program_id;

    Json pruning = Json::object();
    pruning["dpor_schedules_explored"] = dpor_schedules.size();
    pruning["naive_equivalent_schedules"] = naive_schedules.size();
    pruning["pruning_fraction_denominator"] = naive_schedules.size();
    pruning["pruning_fraction_numerator"] = pruned;
    pruning["pruning_percent_milli"] = naive_schedules.empty()
        ? 0
        : (pruned * 100000) / naive_schedules.size();
    pruning["schedules_pruned"] = pruned;
    root["pruning"] = std::move(pruning);

    Json runs = Json::object();
    runs["dpor"] = run_json(dpor, dpor_schedules, pruned);
    runs["naive"] = run_json(naive, naive_schedules, 0);
    root["runs"] = std::move(runs);
    root["schema_version"] = 1;
    root["tree"] = tree_json(checker, naive_schedules, dpor_schedules, pruned);
    return root;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        const Json exported = export_run(options);
        showcase::json::write(std::cout, exported);
        std::cout << '\n';
        return 0;
    } catch (const cli::ParseError& error) {
        std::cerr << "parse error";
        if (error.line() != 0) {
            std::cerr << " at line " << error.line();
        }
        std::cerr << ": " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "showcase export failed: " << error.what() << '\n';
        return 2;
    }
}
