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
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kMaxMeteredNaiveSchedules = 24;
constexpr std::size_t kNoScheduleCap = std::numeric_limits<std::size_t>::max();

enum class ProgramSource { TwoThreadFamily, HandPicked, Fuzz };
enum class VerdictKind { Clean, Race, Deadlock, Error, Assertion, Bound };

struct SourceStats {
    std::size_t metered{0};
};

struct AggregateStats {
    SourceStats two_thread;
    SourceStats hand_picked;
    SourceStats fuzz;
    std::size_t programs_metered{0};
    std::size_t total_classes{0};
    std::size_t total_dpor_schedules{0};
    std::size_t total_naive_schedules{0};
    std::size_t optimal_programs{0};
};

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
                                        const std::vector<model::EffectiveScheduleStep>& trace) {
    const std::vector<std::string> labels = transition_labels(trace);
    std::vector<std::vector<std::size_t>> outgoing(trace.size());
    std::vector<std::size_t> indegree(trace.size(), 0);

    for (std::size_t i = 0; i < trace.size(); ++i) {
        for (std::size_t j = i + 1; j < trace.size(); ++j) {
            // This gate measures optimality against the same transition
            // predicate DPOR actually prunes with: same-thread transitions are
            // ordered, enabled valid Join gets the checker-local ADR 0011
            // refinement, and all other pairs fall back to action-level
            // independent(). A stricter relation would inflate the class count
            // beyond the pruner's stated equivalence relation.
            const bool dependent =
                trace[i].endpoint.thread == trace[j].endpoint.thread ||
                !checker.dpor_transitions_independent(
                    trace[i].endpoint.thread,
                    trace[i].effective_action,
                    trace[j].endpoint.thread,
                    trace[j].effective_action);
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
                            bool enforce_small_schedule_limit) {
    const model::ModelChecker checker(program);
    const model::CheckResult naive = checker.explore_naive(kNoScheduleCap);
    if (!eligible_for_meter(naive, enforce_small_schedule_limit)) {
        return 0;
    }

    const model::CheckResult dpor = checker.explore_dpor(kNoScheduleCap);
    const std::vector<model::Schedule> schedules =
        checker.collect_naive_schedules(naive.schedules_explored + 1);
    if (schedules.size() != naive.schedules_explored) {
        std::cerr << "naive schedule collector mismatch: collected=" << schedules.size()
                  << " naive=" << naive.schedules_explored << '\n';
        print_program(program);
        std::abort();
    }

    std::map<std::vector<std::string>, VerdictKind> class_verdicts;
    for (const model::Schedule& schedule : schedules) {
        const std::vector<model::EffectiveScheduleStep> effective_trace =
            checker.replay_effective_trace(schedule);
        const std::vector<std::string> canonical = canonical_form(checker, effective_trace);
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

} // namespace

int main() {
    AggregateStats stats;
    measure_two_thread_family(stats);
    measure_hand_picked(stats);
    measure_fuzz_sample(stats);
    print_summary(stats);
    return 0;
}
