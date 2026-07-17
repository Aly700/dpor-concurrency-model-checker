#pragma once

#include "model/checker.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace symmetry_diagnosis {

using ThreadPermutation = std::vector<model::ThreadId>;

struct ProgramSymmetry {
    bool has_identical_normalized_bodies{false};
    bool has_identical_nonempty_normalized_bodies{false};
    std::vector<std::vector<model::ThreadId>> identical_body_groups;
    std::vector<std::size_t> identical_group_body_lengths;
    std::size_t maximum_identical_multiplicity{1};
    // Whole-program structural automorphisms before semantic restrictions.
    std::size_t structural_automorphism_count{0};
    // Conservative transition-system automorphisms. Identity is always first.
    // Programs containing Signal currently retain only identity because the
    // interpreter wakes the lowest-numbered waiter.
    std::vector<ThreadPermutation> automorphisms;
    bool restricted_by_signal_priority{false};
};

enum class OrbitExclusion {
    None,
    NoIdenticalBodies,
    NoNontrivialAutomorphism,
    IncompleteExploration,
    CyclePrefix,
    BoundPrefix,
    ErrorOrAssertionPrefix,
    TooManyNaiveSchedules,
    NodeSensitiveTransitionRelation,
    ClassOutcomeViolation,
    ClassLowerBoundViolation,
    ReportIsomorphismViolation,
};

enum class OutcomeSignature : std::uint8_t {
    Clean = 0,
    Race = 1,
    Deadlock = 2,
    RaceAndDeadlock = 3,
};

struct OrbitCounts {
    std::size_t naive_schedules{0};
    std::size_t mazurkiewicz_classes{0};
    std::size_t orbit_classes{0};
};

struct ProgramMeasurement {
    ProgramSymmetry symmetry;
    std::size_t dpor_schedules{0};
    OrbitExclusion exclusion{OrbitExclusion::None};
    std::optional<OrbitCounts> orbit;
};

struct ExplorationNanoseconds {
    std::uint64_t naive{0};
    std::uint64_t dpor{0};
};

ProgramSymmetry analyze_program(const model::Program& program);

OutcomeSignature outcome_signature(const model::CheckResult& result);

bool report_payloads_are_isomorphic(
    const model::Program& program,
    const model::CheckResult& source,
    const model::CheckResult& mapped,
    const ThreadPermutation& permutation);

ProgramMeasurement measure_program(const model::Program& program,
                                   const model::ModelChecker& checker,
                                   model::MemoryModel memory_model,
                                   const model::CheckResult& naive,
                                   const model::CheckResult& dpor,
                                   std::size_t max_naive_schedules);

const char* exclusion_name(OrbitExclusion exclusion);

void record_program(const std::string& corpus,
                    const std::vector<std::string>& tags,
                    const model::Program& program,
                    const model::ModelChecker& checker,
                    model::MemoryModel memory_model,
                    const model::CheckResult& naive,
                    const model::CheckResult& dpor,
                    std::size_t max_naive_schedules,
                    ExplorationNanoseconds timing = {});

void record_dpor_only_program(const std::string& corpus,
                              const std::vector<std::string>& tags,
                              const model::Program& program,
                              const model::CheckResult& dpor,
                              ExplorationNanoseconds timing = {});

std::vector<std::string> summary_lines();
void print_summaries(std::ostream& output);
void reset_corpora();

} // namespace symmetry_diagnosis
