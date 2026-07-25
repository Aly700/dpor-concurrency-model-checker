#include "symmetry_diagnosis.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace symmetry_diagnosis {
namespace {

struct NormalizedBody {
    std::vector<model::Action> actions;
    std::vector<std::uint32_t> raw_indexes;
    std::vector<std::size_t> raw_to_ordinal;
};

using NormalizedProgram = std::vector<NormalizedBody>;

constexpr std::size_t kOrbitExclusionCount =
    static_cast<std::size_t>(OrbitExclusion::ReportIsomorphismViolation) + 1;

struct TagStats {
    std::size_t programs{0};
    std::size_t identical_body_programs{0};
    std::size_t automorphic_programs{0};
};

struct CorpusStats {
    std::size_t programs{0};
    std::size_t identical_body_programs{0};
    std::size_t identical_nonempty_body_programs{0};
    std::size_t structurally_automorphic_programs{0};
    std::size_t automorphic_programs{0};
    std::size_t signal_restricted_programs{0};
    std::size_t dpor_schedules{0};
    std::size_t identical_body_dpor_schedules{0};
    std::size_t automorphic_dpor_schedules{0};
    std::size_t orbit_eligible_programs{0};
    std::size_t orbit_naive_schedules{0};
    std::size_t orbit_dpor_schedules{0};
    std::size_t mazurkiewicz_classes{0};
    std::size_t orbit_classes{0};
    std::size_t maximum_identical_multiplicity{1};
    std::size_t dpor_only_programs{0};
    std::array<std::size_t, kOrbitExclusionCount> exclusions{};
    std::map<std::size_t, std::size_t> candidate_programs_by_thread_count;
    std::map<std::size_t, std::size_t> candidate_groups_by_body_length;
    std::map<std::string, TagStats> tags;
    std::uint64_t total_naive_ns{0};
    std::uint64_t total_dpor_ns{0};
    std::uint64_t identical_body_dpor_ns{0};
    std::uint64_t automorphic_dpor_ns{0};
    std::uint64_t orbit_eligible_dpor_ns{0};
    long double estimated_gross_saved_ns{0};
    long double estimated_symmetry_only_saved_ns{0};
};

std::map<std::string, CorpusStats>& corpora() {
    static std::map<std::string, CorpusStats> value;
    return value;
}

std::optional<std::uint32_t> first_label_index(const std::vector<model::Action>& body,
                                               const std::string& label) {
    for (std::uint32_t index = 0; index < body.size(); ++index) {
        if (body.at(index).kind == model::ActionKind::Label &&
            body.at(index).label == label) {
            return index;
        }
    }
    return std::nullopt;
}

NormalizedBody normalize_body(const std::vector<model::Action>& body) {
    NormalizedBody normalized;
    normalized.raw_to_ordinal.resize(body.size() + 1);

    std::size_t ordinal = 0;
    for (std::size_t raw = 0; raw < body.size(); ++raw) {
        normalized.raw_to_ordinal.at(raw) = ordinal;
        if (body.at(raw).kind != model::ActionKind::Label) {
            ++ordinal;
        }
    }
    normalized.raw_to_ordinal.at(body.size()) = ordinal;

    for (std::uint32_t raw = 0; raw < body.size(); ++raw) {
        const model::Action& source = body.at(raw);
        if (source.kind == model::ActionKind::Label) {
            continue;
        }
        model::Action action = source;
        if (action.kind == model::ActionKind::BranchNonzero) {
            const auto label_index = first_label_index(body, action.label);
            if (label_index.has_value()) {
                action.label = "@" + std::to_string(
                    normalized.raw_to_ordinal.at(*label_index));
            } else {
                // Missing labels are modeled errors whose spelling is public.
                action.label = "!missing:" + action.label;
            }
        }
        normalized.actions.push_back(std::move(action));
        normalized.raw_indexes.push_back(raw);
    }
    return normalized;
}

NormalizedProgram normalize_program(const model::Program& program) {
    NormalizedProgram normalized;
    normalized.reserve(program.threads.size());
    for (const auto& body : program.threads) {
        normalized.push_back(normalize_body(body));
    }
    return normalized;
}

bool same_normalized_body(const NormalizedBody& lhs, const NormalizedBody& rhs) {
    return lhs.actions == rhs.actions;
}

model::Action permute_thread_target(model::Action action,
                                    const ThreadPermutation& permutation) {
    if ((action.kind == model::ActionKind::Spawn ||
         action.kind == model::ActionKind::Join) &&
        action.target < permutation.size()) {
        action.target = permutation.at(action.target);
    }
    return action;
}

bool is_identity(const ThreadPermutation& permutation) {
    for (std::size_t index = 0; index < permutation.size(); ++index) {
        if (permutation.at(index) != index) {
            return false;
        }
    }
    return true;
}

bool structurally_preserves_program(const NormalizedProgram& program,
                                    const ThreadPermutation& permutation) {
    for (std::size_t source = 0; source < program.size(); ++source) {
        std::vector<model::Action> transformed;
        transformed.reserve(program.at(source).actions.size());
        for (const model::Action& action : program.at(source).actions) {
            transformed.push_back(permute_thread_target(action, permutation));
        }
        const std::size_t destination = permutation.at(source);
        if (transformed != program.at(destination).actions) {
            return false;
        }
    }
    return true;
}

bool has_signal(const model::Program& program) {
    for (const auto& body : program.threads) {
        for (const model::Action& action : body) {
            if (action.kind == model::ActionKind::Signal) {
                return true;
            }
        }
    }
    return false;
}

void enumerate_candidate_permutations(
    const std::vector<std::vector<model::ThreadId>>& groups,
    std::size_t group_index,
    ThreadPermutation& current,
    std::vector<ThreadPermutation>& out) {
    if (group_index == groups.size()) {
        out.push_back(current);
        return;
    }

    const auto& sources = groups.at(group_index);
    std::vector<model::ThreadId> destinations = sources;
    do {
        for (std::size_t index = 0; index < sources.size(); ++index) {
            current.at(sources.at(index)) = destinations.at(index);
        }
        enumerate_candidate_permutations(
            groups, group_index + 1, current, out);
    } while (std::next_permutation(destinations.begin(), destinations.end()));
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

std::string register_key(const std::optional<model::RegisterId>& reg) {
    return reg.has_value() ? std::to_string(static_cast<unsigned>(*reg)) : "-";
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
        << "|d=" << register_key(action.destination)
        << "|s=" << register_key(action.source_register)
        << "|v=" << operand_key(action.value)
        << "|e=" << operand_key(action.expected)
        << "|l=" << action.label;
    return out.str();
}

std::string broadcast_waking_set_key(
    const std::optional<std::vector<model::ThreadId>>& waking_set) {
    if (!waking_set.has_value()) {
        return "-";
    }
    std::ostringstream out;
    out << '{';
    for (std::size_t index = 0; index < waking_set->size(); ++index) {
        if (index != 0) {
            out << ',';
        }
        out << waking_set->at(index);
    }
    out << '}';
    return out.str();
}

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

std::vector<std::string> transition_labels(
    const NormalizedProgram& normalized,
    const std::vector<model::EffectiveScheduleStep>& trace) {
    std::map<std::string, std::size_t> occurrences;
    std::vector<std::string> labels;
    labels.reserve(trace.size());
    for (const model::EffectiveScheduleStep& step : trace) {
        std::string pc;
        model::Action effective = step.effective_action;
        if (step.endpoint.action_index == model::kFlushActionIndex) {
            pc = "flush";
        } else {
            const NormalizedBody& body = normalized.at(step.endpoint.thread);
            const std::size_t ordinal =
                body.raw_to_ordinal.at(step.endpoint.action_index);
            pc = std::to_string(ordinal);
            const model::Action& source = body.actions.at(ordinal);
            if (source.kind == model::ActionKind::BranchNonzero) {
                effective.label = source.label;
            }
        }

        std::ostringstream base;
        base << step.endpoint.thread << '|' << pc << '|';
        if (step.endpoint.flush_address.has_value()) {
            base << *step.endpoint.flush_address;
        } else {
            base << '-';
        }
        base << '|' << action_key(effective)
             << "|wake="
             << broadcast_waking_set_key(step.broadcast_waking_set);
        const std::size_t occurrence = occurrences[base.str()]++;

        std::ostringstream label;
        label << "t=" << step.endpoint.thread
              << "|pc=" << pc
              << "|fa=";
        if (step.endpoint.flush_address.has_value()) {
            label << *step.endpoint.flush_address;
        } else {
            label << '-';
        }
        label << "|occ=" << occurrence
              << "|wake="
              << broadcast_waking_set_key(step.broadcast_waking_set)
              << '|' << action_key(effective);
        labels.push_back(label.str());
    }
    return labels;
}

std::vector<std::string> canonical_form(
    const model::ModelChecker& checker,
    model::MemoryModel memory_model,
    const NormalizedProgram& normalized,
    const std::vector<model::EffectiveScheduleStep>& trace) {
    const std::vector<std::string> labels = transition_labels(normalized, trace);
    std::vector<std::vector<std::size_t>> outgoing(trace.size());
    std::vector<std::size_t> indegree(trace.size(), 0);

    for (std::size_t before = 0; before < trace.size(); ++before) {
        for (std::size_t after = before + 1; after < trace.size(); ++after) {
            const bool dependent =
                same_thread_steps_ordered(
                    memory_model, trace.at(before), trace.at(after)) ||
                !checker.dpor_transitions_independent(
                    trace.at(before).endpoint.thread,
                    trace.at(before).effective_action,
                    trace.at(after).endpoint.thread,
                    trace.at(after).effective_action);
            if (!dependent) {
                continue;
            }
            outgoing.at(before).push_back(after);
            ++indegree.at(after);
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
        if (indegree.at(index) == 0) {
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
            if (indegree.at(child) == 0) {
                throw std::logic_error("symmetry diagnosis DAG indegree underflow");
            }
            --indegree.at(child);
            if (indegree.at(child) == 0) {
                available.insert(child);
            }
        }
    }
    if (canonical.size() != trace.size()) {
        throw std::logic_error("symmetry diagnosis dependence graph is cyclic");
    }
    return canonical;
}

model::Schedule permute_schedule(const model::Schedule& schedule,
                                 const NormalizedProgram& normalized,
                                 const ThreadPermutation& permutation) {
    model::Schedule mapped;
    mapped.reserve(schedule.size());
    for (const model::ScheduleStep& source : schedule) {
        model::ScheduleStep destination = source;
        destination.thread = permutation.at(source.thread);
        if (source.action_index != model::kFlushActionIndex) {
            const NormalizedBody& source_body = normalized.at(source.thread);
            const std::size_t ordinal =
                source_body.raw_to_ordinal.at(source.action_index);
            destination.action_index = normalized.at(destination.thread)
                                           .raw_indexes.at(ordinal);
        }
        mapped.push_back(destination);
    }
    return mapped;
}

model::ScheduleStep permute_endpoint(const model::ScheduleStep& endpoint,
                                     const NormalizedProgram& normalized,
                                     const ThreadPermutation& permutation) {
    return permute_schedule({endpoint}, normalized, permutation).front();
}

bool report_payloads_are_isomorphic_normalized(
    const model::CheckResult& source,
    const model::CheckResult& mapped,
    const NormalizedProgram& normalized,
    const ThreadPermutation& permutation) {
    if (source.first_race.has_value() != mapped.first_race.has_value() ||
        source.first_deadlock.has_value() != mapped.first_deadlock.has_value()) {
        return false;
    }
    if (source.first_race.has_value()) {
        const model::RaceReport& lhs = *source.first_race;
        const model::RaceReport& rhs = *mapped.first_race;
        if (lhs.address != rhs.address ||
            permute_endpoint(lhs.first, normalized, permutation) != rhs.first ||
            permute_endpoint(lhs.second, normalized, permutation) != rhs.second ||
            permute_schedule(lhs.schedule, normalized, permutation) !=
                rhs.schedule) {
            return false;
        }
    }
    if (source.first_deadlock.has_value()) {
        std::vector<model::BlockedThread> expected =
            source.first_deadlock->blocked_threads;
        for (model::BlockedThread& blocked : expected) {
            blocked.thread = permutation.at(blocked.thread);
            if (blocked.owner.has_value()) {
                blocked.owner = permutation.at(*blocked.owner);
            }
            if (blocked.target.has_value() && *blocked.target < permutation.size()) {
                blocked.target = permutation.at(*blocked.target);
            }
        }
        std::sort(expected.begin(), expected.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.thread < rhs.thread;
        });
        std::vector<model::BlockedThread> actual =
            mapped.first_deadlock->blocked_threads;
        std::sort(actual.begin(), actual.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.thread < rhs.thread;
        });
        if (expected != actual) {
            return false;
        }
        if (permute_schedule(source.first_deadlock->schedule,
                             normalized,
                             permutation) !=
            mapped.first_deadlock->schedule) {
            return false;
        }
    }
    return true;
}

bool has_node_sensitive_transition(const model::Program& program) {
    for (const auto& body : program.threads) {
        for (const model::Action& action : body) {
            if (action.kind == model::ActionKind::TryLock ||
                action.kind == model::ActionKind::BarrierWait) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

ProgramSymmetry analyze_program(const model::Program& program) {
    const NormalizedProgram normalized = normalize_program(program);
    ProgramSymmetry result;

    std::vector<bool> grouped(normalized.size(), false);
    for (std::size_t first = 0; first < normalized.size(); ++first) {
        if (grouped.at(first)) {
            continue;
        }
        std::vector<model::ThreadId> group{
            static_cast<model::ThreadId>(first)};
        for (std::size_t candidate = first + 1; candidate < normalized.size();
             ++candidate) {
            if (!grouped.at(candidate) &&
                same_normalized_body(normalized.at(first),
                                     normalized.at(candidate))) {
                group.push_back(static_cast<model::ThreadId>(candidate));
                grouped.at(candidate) = true;
            }
        }
        if (group.size() >= 2) {
            grouped.at(first) = true;
            result.maximum_identical_multiplicity =
                std::max(result.maximum_identical_multiplicity, group.size());
            result.identical_group_body_lengths.push_back(
                normalized.at(first).actions.size());
            if (!normalized.at(first).actions.empty()) {
                result.has_identical_nonempty_normalized_bodies = true;
            }
            result.identical_body_groups.push_back(std::move(group));
        }
    }
    result.has_identical_normalized_bodies =
        !result.identical_body_groups.empty();

    ThreadPermutation identity(program.threads.size());
    for (std::size_t index = 0; index < identity.size(); ++index) {
        identity.at(index) = static_cast<model::ThreadId>(index);
    }
    std::vector<ThreadPermutation> candidates;
    enumerate_candidate_permutations(
        result.identical_body_groups, 0, identity, candidates);

    const bool signal_priority = has_signal(program);
    for (const ThreadPermutation& candidate : candidates) {
        if (!structurally_preserves_program(normalized, candidate)) {
            continue;
        }
        ++result.structural_automorphism_count;
        if (!signal_priority || is_identity(candidate)) {
            result.automorphisms.push_back(candidate);
        } else {
            result.restricted_by_signal_priority = true;
        }
    }
    if (result.structural_automorphism_count == 0) {
        throw std::logic_error("identity failed whole-program symmetry validation");
    }
    if (result.automorphisms.empty()) {
        throw std::logic_error("identity missing from symmetry automorphisms");
    }
    return result;
}

OutcomeSignature outcome_signature(const model::CheckResult& result) {
    const std::uint8_t race = result.first_race.has_value() ? 1 : 0;
    const std::uint8_t deadlock = result.first_deadlock.has_value() ? 2 : 0;
    return static_cast<OutcomeSignature>(race | deadlock);
}

bool report_payloads_are_isomorphic(
    const model::Program& program,
    const model::CheckResult& source,
    const model::CheckResult& mapped,
    const ThreadPermutation& permutation) {
    if (permutation.size() != program.threads.size()) {
        return false;
    }
    return report_payloads_are_isomorphic_normalized(
        source, mapped, normalize_program(program), permutation);
}

ProgramMeasurement measure_program(const model::Program& program,
                                   const model::ModelChecker& checker,
                                   model::MemoryModel memory_model,
                                   const model::CheckResult& naive,
                                   const model::CheckResult& dpor,
                                   std::size_t max_naive_schedules) {
    ProgramMeasurement measurement;
    measurement.symmetry = analyze_program(program);
    measurement.dpor_schedules = dpor.schedules_explored;
    if (!measurement.symmetry.has_identical_normalized_bodies) {
        measurement.exclusion = OrbitExclusion::NoIdenticalBodies;
        return measurement;
    }
    if (measurement.symmetry.automorphisms.size() < 2) {
        measurement.exclusion = OrbitExclusion::NoNontrivialAutomorphism;
        return measurement;
    }
    if (naive.exploration_capped || dpor.exploration_capped) {
        measurement.exclusion = OrbitExclusion::IncompleteExploration;
        return measurement;
    }
    if (naive.cycles_detected != 0 || dpor.cycles_detected != 0) {
        measurement.exclusion = OrbitExclusion::CyclePrefix;
        return measurement;
    }
    if (naive.bound_exceeded_executions != 0 ||
        dpor.bound_exceeded_executions != 0) {
        measurement.exclusion = OrbitExclusion::BoundPrefix;
        return measurement;
    }
    if (naive.first_error.has_value() || dpor.first_error.has_value() ||
        naive.first_assertion.has_value() || dpor.first_assertion.has_value()) {
        measurement.exclusion = OrbitExclusion::ErrorOrAssertionPrefix;
        return measurement;
    }
    if (naive.schedules_explored > max_naive_schedules) {
        measurement.exclusion = OrbitExclusion::TooManyNaiveSchedules;
        return measurement;
    }
    if (has_node_sensitive_transition(program)) {
        measurement.exclusion = OrbitExclusion::NodeSensitiveTransitionRelation;
        return measurement;
    }

    const NormalizedProgram normalized = normalize_program(program);
    const std::vector<model::Schedule> schedules =
        checker.collect_naive_schedules(naive.schedules_explored + 1);
    if (schedules.size() != naive.schedules_explored) {
        measurement.exclusion = OrbitExclusion::IncompleteExploration;
        return measurement;
    }

    std::map<std::vector<std::string>, OutcomeSignature> class_outcomes;
    std::set<std::vector<std::string>> orbits;
    for (const model::Schedule& schedule : schedules) {
        const model::CheckResult source_result = checker.replay(schedule);
        const auto source_trace = checker.replay_effective_trace(schedule);
        const auto source_class =
            canonical_form(checker, memory_model, normalized, source_trace);
        const OutcomeSignature source_outcome = outcome_signature(source_result);
        const auto [class_position, class_inserted] =
            class_outcomes.emplace(source_class, source_outcome);
        if (!class_inserted && class_position->second != source_outcome) {
            measurement.exclusion = OrbitExclusion::ClassOutcomeViolation;
            return measurement;
        }

        std::optional<std::vector<std::string>> orbit_key;
        for (const ThreadPermutation& permutation :
             measurement.symmetry.automorphisms) {
            const model::Schedule mapped_schedule =
                permute_schedule(schedule, normalized, permutation);
            model::CheckResult mapped_result;
            std::vector<model::EffectiveScheduleStep> mapped_trace;
            try {
                mapped_result = checker.replay(mapped_schedule);
                mapped_trace = checker.replay_effective_trace(mapped_schedule);
            } catch (const std::exception&) {
                measurement.exclusion =
                    OrbitExclusion::ReportIsomorphismViolation;
                return measurement;
            }
            if (source_outcome != outcome_signature(mapped_result) ||
                !report_payloads_are_isomorphic_normalized(
                    source_result, mapped_result, normalized, permutation)) {
                measurement.exclusion =
                    OrbitExclusion::ReportIsomorphismViolation;
                return measurement;
            }
            const auto candidate =
                canonical_form(checker, memory_model, normalized, mapped_trace);
            if (!orbit_key.has_value() || candidate < *orbit_key) {
                orbit_key = candidate;
            }
        }
        if (!orbit_key.has_value()) {
            throw std::logic_error("symmetry orbit lacks identity representative");
        }
        orbits.insert(std::move(*orbit_key));
    }

    if (class_outcomes.size() > dpor.schedules_explored ||
        dpor.schedules_explored > naive.schedules_explored ||
        orbits.size() > class_outcomes.size()) {
        measurement.exclusion = OrbitExclusion::ClassLowerBoundViolation;
        return measurement;
    }
    measurement.orbit = OrbitCounts{
        naive.schedules_explored,
        class_outcomes.size(),
        orbits.size(),
    };
    measurement.exclusion = OrbitExclusion::None;
    return measurement;
}

const char* exclusion_name(OrbitExclusion exclusion) {
    switch (exclusion) {
    case OrbitExclusion::None:
        return "none";
    case OrbitExclusion::NoIdenticalBodies:
        return "no-identical-bodies";
    case OrbitExclusion::NoNontrivialAutomorphism:
        return "no-nontrivial-automorphism";
    case OrbitExclusion::IncompleteExploration:
        return "incomplete-exploration";
    case OrbitExclusion::CyclePrefix:
        return "cycle-prefix";
    case OrbitExclusion::BoundPrefix:
        return "bound-prefix";
    case OrbitExclusion::ErrorOrAssertionPrefix:
        return "error-or-assertion-prefix";
    case OrbitExclusion::TooManyNaiveSchedules:
        return "too-many-naive-schedules";
    case OrbitExclusion::NodeSensitiveTransitionRelation:
        return "node-sensitive-transition-relation";
    case OrbitExclusion::ClassOutcomeViolation:
        return "class-outcome-violation";
    case OrbitExclusion::ClassLowerBoundViolation:
        return "class-lower-bound-violation";
    case OrbitExclusion::ReportIsomorphismViolation:
        return "report-isomorphism-violation";
    }
    return "unknown";
}

namespace {

void add_tags(CorpusStats& stats,
              const std::vector<std::string>& tags,
              const ProgramSymmetry& symmetry) {
    for (const std::string& tag : tags) {
        TagStats& tagged = stats.tags[tag];
        ++tagged.programs;
        if (symmetry.has_identical_normalized_bodies) {
            ++tagged.identical_body_programs;
        }
        if (symmetry.automorphisms.size() >= 2) {
            ++tagged.automorphic_programs;
        }
    }
}

void add_common(CorpusStats& stats,
                const std::vector<std::string>& tags,
                const model::Program& program,
                const ProgramSymmetry& symmetry,
                const model::CheckResult& dpor,
                ExplorationNanoseconds timing) {
    ++stats.programs;
    stats.dpor_schedules += dpor.schedules_explored;
    stats.total_naive_ns += timing.naive;
    stats.total_dpor_ns += timing.dpor;
    stats.maximum_identical_multiplicity = std::max(
        stats.maximum_identical_multiplicity,
        symmetry.maximum_identical_multiplicity);
    add_tags(stats, tags, symmetry);

    if (!symmetry.has_identical_normalized_bodies) {
        return;
    }
    ++stats.identical_body_programs;
    stats.identical_body_dpor_schedules += dpor.schedules_explored;
    stats.identical_body_dpor_ns += timing.dpor;
    ++stats.candidate_programs_by_thread_count[program.threads.size()];
    for (const std::size_t length : symmetry.identical_group_body_lengths) {
        ++stats.candidate_groups_by_body_length[length];
    }
    if (symmetry.has_identical_nonempty_normalized_bodies) {
        ++stats.identical_nonempty_body_programs;
    }
    if (symmetry.restricted_by_signal_priority) {
        ++stats.signal_restricted_programs;
    }
    if (symmetry.structural_automorphism_count >= 2) {
        ++stats.structurally_automorphic_programs;
    }
    if (symmetry.automorphisms.size() >= 2) {
        ++stats.automorphic_programs;
        stats.automorphic_dpor_schedules += dpor.schedules_explored;
        stats.automorphic_dpor_ns += timing.dpor;
    }
}

std::string histogram(const std::map<std::size_t, std::size_t>& values) {
    if (values.empty()) {
        return "-";
    }
    std::ostringstream out;
    bool first = true;
    for (const auto& [key, value] : values) {
        if (!first) {
            out << ',';
        }
        first = false;
        out << key << ':' << value;
    }
    return out.str();
}

std::string tag_summary(const std::map<std::string, TagStats>& tags) {
    if (tags.empty()) {
        return "-";
    }
    std::ostringstream out;
    bool first = true;
    for (const auto& [name, stats] : tags) {
        if (!first) {
            out << ',';
        }
        first = false;
        out << name << ':' << stats.identical_body_programs << '/'
            << stats.automorphic_programs << '/' << stats.programs;
    }
    return out.str();
}

std::uint64_t rounded_nanoseconds(long double value) {
    if (value <= 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(std::llround(value));
}

} // namespace

void record_program(const std::string& corpus,
                    const std::vector<std::string>& tags,
                    const model::Program& program,
                    const model::ModelChecker& checker,
                    model::MemoryModel memory_model,
                    const model::CheckResult& naive,
                    const model::CheckResult& dpor,
                    std::size_t max_naive_schedules,
                    ExplorationNanoseconds timing) {
    const ProgramMeasurement measurement = measure_program(
        program,
        checker,
        memory_model,
        naive,
        dpor,
        max_naive_schedules);
    CorpusStats& stats = corpora()[corpus];
    add_common(stats, tags, program, measurement.symmetry, dpor, timing);
    ++stats.exclusions.at(static_cast<std::size_t>(measurement.exclusion));
    if (!measurement.orbit.has_value()) {
        return;
    }

    ++stats.orbit_eligible_programs;
    stats.orbit_naive_schedules += measurement.orbit->naive_schedules;
    stats.orbit_dpor_schedules += dpor.schedules_explored;
    stats.mazurkiewicz_classes += measurement.orbit->mazurkiewicz_classes;
    stats.orbit_classes += measurement.orbit->orbit_classes;
    stats.orbit_eligible_dpor_ns += timing.dpor;
    if (dpor.schedules_explored != 0) {
        const long double dpor_count = dpor.schedules_explored;
        stats.estimated_gross_saved_ns +=
            static_cast<long double>(timing.dpor) *
            (dpor_count - measurement.orbit->orbit_classes) / dpor_count;
        stats.estimated_symmetry_only_saved_ns +=
            static_cast<long double>(timing.dpor) *
            (static_cast<long double>(measurement.orbit->mazurkiewicz_classes) -
             measurement.orbit->orbit_classes) /
            dpor_count;
    }
}

void record_dpor_only_program(const std::string& corpus,
                              const std::vector<std::string>& tags,
                              const model::Program& program,
                              const model::CheckResult& dpor,
                              ExplorationNanoseconds timing) {
    const ProgramSymmetry symmetry = analyze_program(program);
    CorpusStats& stats = corpora()[corpus];
    add_common(stats, tags, program, symmetry, dpor, timing);
    ++stats.dpor_only_programs;
    ++stats.exclusions.at(
        static_cast<std::size_t>(OrbitExclusion::IncompleteExploration));
}

std::vector<std::string> summary_lines() {
    std::vector<std::string> lines;
    lines.reserve(corpora().size() * 2);
    for (const auto& [corpus, stats] : corpora()) {
        const long double prevalence = stats.programs == 0
            ? 0
            : 100.0L * stats.identical_body_programs / stats.programs;
        const long double automorphic_prevalence = stats.programs == 0
            ? 0
            : 100.0L * stats.automorphic_programs / stats.programs;
        std::ostringstream counts;
        counts << std::fixed << std::setprecision(3)
               << "symmetry_diagnosis: corpus=" << corpus
               << " programs=" << stats.programs
               << " identical_body_programs=" << stats.identical_body_programs
               << " automorphic_programs=" << stats.automorphic_programs
               << " structurally_automorphic_programs="
               << stats.structurally_automorphic_programs
               << " identical_nonempty_body_programs="
               << stats.identical_nonempty_body_programs
               << " prevalence_percent=" << prevalence
               << " automorphic_prevalence_percent=" << automorphic_prevalence
               << " maximum_multiplicity=" << stats.maximum_identical_multiplicity
               << " total_dpor_schedules=" << stats.dpor_schedules
               << " identical_body_dpor_schedules="
               << stats.identical_body_dpor_schedules
               << " automorphic_dpor_schedules="
               << stats.automorphic_dpor_schedules
               << " orbit_eligible_programs=" << stats.orbit_eligible_programs
               << " orbit_naive_schedules=" << stats.orbit_naive_schedules
               << " orbit_dpor_schedules=" << stats.orbit_dpor_schedules
               << " mazurkiewicz_classes=" << stats.mazurkiewicz_classes
               << " orbit_classes=" << stats.orbit_classes
               << " gross_removable_representatives="
               << stats.orbit_dpor_schedules - stats.orbit_classes
               << " symmetry_only_removed_classes="
               << stats.mazurkiewicz_classes - stats.orbit_classes
               << " dpor_only_programs=" << stats.dpor_only_programs
               << " signal_restricted_programs="
               << stats.signal_restricted_programs
               << " excluded_no_automorphism="
               << stats.exclusions.at(static_cast<std::size_t>(
                      OrbitExclusion::NoNontrivialAutomorphism))
               << " excluded_incomplete="
               << stats.exclusions.at(static_cast<std::size_t>(
                      OrbitExclusion::IncompleteExploration))
               << " excluded_cycle="
               << stats.exclusions.at(static_cast<std::size_t>(
                      OrbitExclusion::CyclePrefix))
               << " excluded_bound="
               << stats.exclusions.at(static_cast<std::size_t>(
                      OrbitExclusion::BoundPrefix))
               << " excluded_error_assertion="
               << stats.exclusions.at(static_cast<std::size_t>(
                      OrbitExclusion::ErrorOrAssertionPrefix))
               << " excluded_too_large="
               << stats.exclusions.at(static_cast<std::size_t>(
                      OrbitExclusion::TooManyNaiveSchedules))
               << " excluded_node_sensitive="
               << stats.exclusions.at(static_cast<std::size_t>(
                      OrbitExclusion::NodeSensitiveTransitionRelation))
               << " excluded_class_outcome_violation="
               << stats.exclusions.at(static_cast<std::size_t>(
                      OrbitExclusion::ClassOutcomeViolation))
               << " excluded_class_violation="
               << stats.exclusions.at(static_cast<std::size_t>(
                      OrbitExclusion::ClassLowerBoundViolation))
               << " excluded_report_violation="
               << stats.exclusions.at(static_cast<std::size_t>(
                      OrbitExclusion::ReportIsomorphismViolation))
               << " candidate_programs_by_thread_count="
               << histogram(stats.candidate_programs_by_thread_count)
               << " candidate_groups_by_body_length="
               << histogram(stats.candidate_groups_by_body_length)
               << " tags_candidate/automorphic/total="
               << tag_summary(stats.tags);
        lines.push_back(counts.str());

        std::ostringstream timing;
        timing << "symmetry_timing: corpus=" << corpus
               << " total_naive_ns=" << stats.total_naive_ns
               << " total_dpor_ns=" << stats.total_dpor_ns
               << " identical_body_dpor_ns=" << stats.identical_body_dpor_ns
               << " automorphic_dpor_ns=" << stats.automorphic_dpor_ns
               << " orbit_eligible_dpor_ns=" << stats.orbit_eligible_dpor_ns
               << " estimated_gross_saved_ns="
               << rounded_nanoseconds(stats.estimated_gross_saved_ns)
               << " estimated_symmetry_only_saved_ns="
               << rounded_nanoseconds(stats.estimated_symmetry_only_saved_ns);
        lines.push_back(timing.str());
    }
    return lines;
}

void print_summaries(std::ostream& output) {
    for (const std::string& line : summary_lines()) {
        output << line << '\n';
    }
}

void reset_corpora() {
    corpora().clear();
}

} // namespace symmetry_diagnosis
