#include "report.hpp"

#include <map>
#include <ostream>
#include <sstream>
#include <utility>

namespace cli {
namespace {

const model::Schedule& bug_schedule(const model::CheckResult& result) {
    if (result.first_race.has_value()) {
        return result.first_race->schedule;
    }
    if (result.first_deadlock.has_value()) {
        return result.first_deadlock->schedule;
    }
    return result.first_error->schedule;
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
    output << "thread " << step.thread << " action " << step.action_index;
    return output.str();
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
    }
    output << '\n';
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
    }
}

void print_trace(std::ostream& output,
                 const model::Program& program,
                 const model::CheckResult& result,
                 const model::Schedule& schedule) {
    output << "trace:\n";
    std::map<std::pair<model::ThreadId, std::uint32_t>, std::size_t> wait_occurrences;
    for (std::size_t index = 0; index < schedule.size(); ++index) {
        const model::ScheduleStep& step = schedule[index];
        const model::Action& action = program.threads.at(step.thread).at(step.action_index);
        output << "  " << index << ": thread " << step.thread
               << " action " << step.action_index << " " << action_text(action);

        if (action.kind == model::ActionKind::Wait &&
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
        output << "  " << step.thread << " " << step.action_index << '\n';
    }
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
    return "clean";
}

bool has_bug(const model::CheckResult& result) {
    return result.first_race.has_value() ||
           result.first_deadlock.has_value() ||
           result.first_error.has_value();
}

void print_report(std::ostream& output, const model::Program& program, const model::CheckResult& result) {
    output << "verdict: " << verdict_of(result) << '\n';
    output << "schedules_explored: " << result.schedules_explored << '\n';
    if (!has_bug(result)) {
        return;
    }

    print_bug_details(output, result);
    const model::Schedule& schedule = bug_schedule(result);
    print_trace(output, program, result, schedule);
    print_schedule(output, schedule);
}

} // namespace cli
