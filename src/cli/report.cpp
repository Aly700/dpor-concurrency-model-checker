#include "report.hpp"

#include <map>
#include <ostream>
#include <sstream>
#include <utility>
#include <vector>

namespace cli {
namespace {

const model::Schedule& bug_schedule(const model::CheckResult& result) {
    if (result.first_race.has_value()) {
        return result.first_race->schedule;
    }
    if (result.first_deadlock.has_value()) {
        return result.first_deadlock->schedule;
    }
    if (result.first_error.has_value()) {
        return result.first_error->schedule;
    }
    if (result.first_assertion.has_value()) {
        return result.first_assertion->schedule;
    }
    return result.first_nontermination->schedule;
}

bool is_selected_error_endpoint(const model::CheckResult& result,
                                const model::ScheduleStep& step,
                                std::size_t step_index,
                                const model::Schedule& schedule) {
    return result.first_error.has_value() &&
           step_index + 1 == schedule.size() &&
           result.first_error->endpoint == step;
}

std::string endpoint_text(const model::ScheduleStep& step) {
    std::ostringstream output;
    output << "thread " << step.thread;
    if (step.action_index == model::kFlushActionIndex) {
        output << " flush";
    } else {
        output << " action " << step.action_index;
    }
    return output.str();
}

void print_numeric_step(std::ostream& output,
                        const char* indentation,
                        const model::ScheduleStep& step) {
    output << indentation << step.thread << ' ' << step.action_index;
    if (step.flush_address.has_value()) {
        output << ' ' << *step.flush_address;
    }
    output << '\n';
}

void print_deadlock_blocker(std::ostream& output, const model::BlockedThread& blocked) {
    output << "    thread " << blocked.thread << ": ";
    switch (blocked.kind) {
    case model::BlockedOnKind::Mutex:
        output << "mutex " << blocked.mutex << " owned_by ";
        if (blocked.owner.has_value()) {
            output << *blocked.owner;
        } else {
            output << "none";
        }
        break;
    case model::BlockedOnKind::Thread:
        output << "thread " << blocked.target.value_or(0);
        break;
    case model::BlockedOnKind::ConditionVariable:
        output << "condition " << blocked.condition << " mutex " << blocked.mutex;
        break;
    case model::BlockedOnKind::RwLockWriter:
        output << "rwlock " << blocked.rwlock << " waiting_for_writer owned_by ";
        if (blocked.owner.has_value()) {
            output << *blocked.owner;
        } else {
            output << "none";
        }
        break;
    case model::BlockedOnKind::RwLockReaders:
        output << "rwlock " << blocked.rwlock << " waiting_for_readers_to_drain";
        if (blocked.self_wait) {
            output << " self_wait";
        }
        break;
    case model::BlockedOnKind::Semaphore:
        output << "semaphore " << blocked.semaphore << " waiting_for_post";
        break;
    case model::BlockedOnKind::Barrier:
        output << "barrier " << blocked.barrier << " waiting_on_barrier";
        break;
    }
    output << '\n';
}

const char* fairness_text(model::Fairness fairness) {
    switch (fairness) {
    case model::Fairness::UnfairScheduleWitness:
        return "unfair-schedule witness";
    case model::Fairness::FairDivergence:
        return "fair divergence";
    }
    return "unknown";
}

// The verdict and detail block show only the highest-priority bug kind, but
// a program can exhibit several kinds at once (e.g. the weak-memory litmus programs
// race by construction AND reach the relaxed-outcome assertion). Listing the
// other kinds keeps cross-model comparisons honest at the CLI: a cross-model
// discriminator on such programs is exactly whether 'assertion' appears here.
void print_also_found(std::ostream& output, const model::CheckResult& result) {
    std::vector<const char*> also;
    bool primary_seen = false;
    const auto consider = [&](bool present, const char* name) {
        if (!present) {
            return;
        }
        if (!primary_seen) {
            primary_seen = true;
            return;
        }
        also.push_back(name);
    };
    consider(result.first_race.has_value(), "race");
    consider(result.first_deadlock.has_value(), "deadlock");
    consider(result.first_error.has_value(), "error");
    consider(result.first_assertion.has_value(), "assertion");
    consider(result.first_nontermination.has_value(), "nontermination");
    for (const char* name : also) {
        output << "also_found: " << name << '\n';
    }
}

void print_bug_details(std::ostream& output, const model::CheckResult& result) {
    if (result.first_race.has_value()) {
        output << "race:\n";
        output << "  address: " << result.first_race->address << '\n';
        output << "  first: " << endpoint_text(result.first_race->first) << '\n';
        output << "  second: " << endpoint_text(result.first_race->second) << '\n';
        return;
    }
    if (result.first_deadlock.has_value()) {
        output << "deadlock:\n";
        output << "  blocked:\n";
        for (const model::BlockedThread& blocked : result.first_deadlock->blocked_threads) {
            print_deadlock_blocker(output, blocked);
        }
        return;
    }
    if (result.first_error.has_value()) {
        output << "error:\n";
        output << "  endpoint: " << endpoint_text(result.first_error->endpoint) << '\n';
        output << "  message: " << result.first_error->message << '\n';
        return;
    }
    if (result.first_assertion.has_value()) {
        output << "assertion:\n";
        output << "  endpoint: " << endpoint_text(result.first_assertion->endpoint) << '\n';
        output << "  register: r" << static_cast<unsigned>(result.first_assertion->reg) << '\n';
        output << "  value: " << result.first_assertion->value << '\n';
        return;
    }
    if (result.first_nontermination.has_value()) {
        output << "nontermination:\n";
        output << "  fairness: " << fairness_text(result.first_nontermination->fairness) << '\n';
        output << "  stem:\n";
        for (const model::ScheduleStep& step : result.first_nontermination->stem) {
            print_numeric_step(output, "    ", step);
        }
        output << "  cycle:\n";
        for (const model::ScheduleStep& step : result.first_nontermination->cycle) {
            print_numeric_step(output, "    ", step);
        }
    }
}

void print_trace(std::ostream& output,
                 const model::Program& program,
                 const model::CheckResult& result,
                 const model::Schedule& schedule,
                 model::MemoryModel memory_model,
                 std::size_t step_bound) {
    output << "trace:\n";
    const std::vector<model::EffectiveScheduleStep> effective_trace =
        model::ModelChecker(program, step_bound, memory_model).replay_effective_trace(schedule);
    std::map<std::pair<model::ThreadId, std::uint32_t>, std::size_t> wait_occurrences;
    for (std::size_t index = 0; index < schedule.size(); ++index) {
        const model::ScheduleStep& step = schedule[index];
        const model::Action& effective_action = effective_trace.at(index).effective_action;
        const bool flush = step.action_index == model::kFlushActionIndex;
        const model::Action& action = flush
            ? effective_action
            : program.threads.at(step.thread).at(step.action_index);
        output << "  " << index << ": thread " << step.thread;
        if (flush) {
            output << " flush " << action.address;
        } else {
            output << " action " << step.action_index << " " << action_text(action);
        }

        if (!flush &&
            action.kind == model::ActionKind::Wait &&
            !is_selected_error_endpoint(result, step, index, schedule)) {
            auto key = std::make_pair(step.thread, step.action_index);
            const std::size_t occurrence = wait_occurrences[key]++;
            if (occurrence == 0) {
                output << " (sleep)";
            } else {
                output << " (reacquire)";
            }
        }
        output << '\n';
    }
}

void print_schedule(std::ostream& output, const model::Schedule& schedule) {
    output << "schedule:\n";
    for (const model::ScheduleStep& step : schedule) {
        print_numeric_step(output, "  ", step);
    }
}

const char* memory_model_text(model::MemoryModel memory_model) {
    switch (memory_model) {
    case model::MemoryModel::SC:
        return "sc";
    case model::MemoryModel::TSO:
        return "tso";
    case model::MemoryModel::PSO:
        return "pso";
    }
    return "unknown";
}

} // namespace

std::string verdict_of(const model::CheckResult& result) {
    if (result.first_race.has_value()) {
        return "race";
    }
    if (result.first_deadlock.has_value()) {
        return "deadlock";
    }
    if (result.first_error.has_value()) {
        return "error";
    }
    if (result.first_assertion.has_value()) {
        return "assertion";
    }
    if (result.first_nontermination.has_value()) {
        return "nontermination";
    }
    if (result.bound_exceeded_executions > 0) {
        return "clean up to bound";
    }
    return "clean";
}

bool has_bug(const model::CheckResult& result) {
    return result.first_race.has_value() ||
           result.first_deadlock.has_value() ||
           result.first_error.has_value() ||
           result.first_assertion.has_value() ||
           result.first_nontermination.has_value();
}

void print_report(std::ostream& output, const model::Program& program, const model::CheckResult& result) {
    print_report(output, program, result, model::MemoryModel::SC, model::ModelChecker::kDefaultStepBound);
}

void print_report(std::ostream& output,
                  const model::Program& program,
                  const model::CheckResult& result,
                  model::MemoryModel memory_model,
                  std::size_t step_bound) {
    output << "verdict: " << verdict_of(result) << '\n';
    if (memory_model != model::MemoryModel::SC) {
        output << "memory_model: " << memory_model_text(memory_model) << '\n';
    }
    output << "schedules_explored: " << result.schedules_explored << '\n';
    if (result.cycles_detected > 0) {
        output << "cycles_detected: " << result.cycles_detected << '\n';
    }
    if (result.bound_exceeded_executions > 0) {
        output << "bound_exceeded_executions: " << result.bound_exceeded_executions << '\n';
    }
    if (result.exploration_capped) {
        // A capped exploration is not a verified verdict: unexplored
        // schedules may hold a bug. Never let a clean line stand alone here.
        output << "exploration_capped: true (schedule cap reached; verdict may be incomplete)\n";
    }
    if (!has_bug(result)) {
        return;
    }

    print_also_found(output, result);
    print_bug_details(output, result);
    const model::Schedule& schedule = bug_schedule(result);
    print_trace(output, program, result, schedule, memory_model, step_bound);
    print_schedule(output, schedule);
}

} // namespace cli
