#include "symmetry_diagnosis.hpp"

#include "model/checker.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using model::Action;
using model::ActionKind;
using model::Program;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

Action label(std::string name) {
    Action action;
    action.kind = ActionKind::Label;
    action.label = std::move(name);
    return action;
}

Action branch(std::string target) {
    Action action;
    action.kind = ActionKind::BranchNonzero;
    action.source_register = 0;
    action.label = std::move(target);
    return action;
}

Action yield() {
    Action action;
    action.kind = ActionKind::Yield;
    return action;
}

Action write(std::string address) {
    Action action;
    action.kind = ActionKind::Write;
    action.address = std::move(address);
    return action;
}

Action join(model::ThreadId target) {
    Action action;
    action.kind = ActionKind::Join;
    action.target = target;
    return action;
}

model::Schedule swap_label_shifted_two_thread_schedule(
    const model::Schedule& schedule) {
    model::Schedule mapped;
    mapped.reserve(schedule.size());
    for (const model::ScheduleStep& source : schedule) {
        model::ScheduleStep destination = source;
        destination.thread = source.thread == 0 ? 1 : 0;
        if (source.action_index != model::kFlushActionIndex) {
            if (source.thread == 0) {
                destination.action_index = source.action_index == 1 ? 0 : 2;
            } else {
                destination.action_index = source.action_index == 0 ? 1 : 3;
            }
        }
        mapped.push_back(destination);
    }
    return mapped;
}

Action lock(std::string mutex) {
    Action action;
    action.kind = ActionKind::Lock;
    action.mutex = std::move(mutex);
    return action;
}

Action wait(std::string condition, std::string mutex) {
    Action action;
    action.kind = ActionKind::Wait;
    action.condition = std::move(condition);
    action.mutex = std::move(mutex);
    return action;
}

Action signal(std::string condition) {
    Action action;
    action.kind = ActionKind::Signal;
    action.condition = std::move(condition);
    return action;
}

Action rlock(std::string name) {
    Action action;
    action.kind = ActionKind::RLock;
    action.rwlock = std::move(name);
    return action;
}

Action read(std::string address) {
    Action action;
    action.kind = ActionKind::Read;
    action.address = std::move(address);
    return action;
}

Action runlock(std::string name) {
    Action action;
    action.kind = ActionKind::RUnlock;
    action.rwlock = std::move(name);
    return action;
}

void label_names_and_positions_are_normalized() {
    const Program program{{
        {label("left"), yield(), label("again"), branch("left")},
        {label("renamed"), label("padding"), yield(), branch("renamed")},
    }};
    const auto symmetry = symmetry_diagnosis::analyze_program(program);
    require(symmetry.has_identical_normalized_bodies,
            "label spelling/placement should not distinguish equal executable bodies");
    require(symmetry.identical_body_groups.size() == 1 &&
                symmetry.identical_body_groups.front().size() == 2,
            "the two label-normalized threads should form one equality group");
    require(symmetry.automorphisms.size() == 2,
            "two isolated equal bodies should admit identity and swap");
}

void external_thread_references_can_break_body_candidate_symmetry() {
    const Program program{{
        {yield()},
        {yield()},
        {join(0)},
    }};
    const auto symmetry = symmetry_diagnosis::analyze_program(program);
    require(symmetry.has_identical_normalized_bodies,
            "equal bodies remain a syntactic symmetry candidate");
    require(symmetry.automorphisms.size() == 1,
            "a third thread targeting only t0 must invalidate the t0/t1 swap");
}

void a_fixed_external_join_target_preserves_the_worker_swap() {
    const Program program{{
        {label("before"), join(2)},
        {join(2), label("after")},
        {yield()},
    }};
    const auto symmetry = symmetry_diagnosis::analyze_program(program);
    require(symmetry.has_identical_normalized_bodies,
            "label-normalized workers with one fixed join target are candidates");
    require(symmetry.automorphisms.size() == 2,
            "swapping workers that both join fixed t2 should be accepted");
}

void lowest_waiter_signal_priority_blocks_thread_swaps() {
    const Program program{{
        {wait("cv", "m")},
        {wait("cv", "m")},
        {signal("cv")},
    }};
    const auto symmetry = symmetry_diagnosis::analyze_program(program);
    require(symmetry.structural_automorphism_count == 2,
            "the program graph alone should admit the waiter swap");
    require(symmetry.automorphisms.size() == 1 &&
                symmetry.restricted_by_signal_priority,
            "lowest-numeric waiter semantics must conservatively remove the swap");
}

void resource_renaming_is_outside_exact_body_symmetry() {
    const Program program{{
        {lock("fork0")},
        {lock("fork1")},
    }};
    const auto symmetry = symmetry_diagnosis::analyze_program(program);
    require(!symmetry.has_identical_normalized_bodies,
            "rotated resource names require a broader graph automorphism scope");
}

void dependent_equal_writes_have_one_orbit_but_two_trace_classes() {
    const Program program{{{write("x")}, {write("x")}}};
    const model::ModelChecker checker(program);
    const model::CheckResult naive = checker.explore_naive();
    const model::CheckResult dpor = checker.explore_dpor();
    const auto measurement = symmetry_diagnosis::measure_program(
        program,
        checker,
        model::MemoryModel::SC,
        naive,
        dpor,
        100);
    require(measurement.orbit.has_value(),
            "the two-write fixture should be eligible for orbit counting");
    require(measurement.orbit->mazurkiewicz_classes == 2,
            "dependent write orders must remain distinct Mazurkiewicz classes");
    require(measurement.orbit->orbit_classes == 1,
            "swapping the identical writer identities should merge the two classes");
    require(dpor.schedules_explored == 2,
            "current DPOR should retain both dependent writer orders");
}

void report_schedules_are_checked_in_original_raw_coordinates() {
    const symmetry_diagnosis::ThreadPermutation swap{1, 0};

    const Program race_program{{
        {label("before"), write("x")},
        {write("x"), label("after")},
    }};
    const model::ModelChecker race_checker(race_program);
    const model::Schedule race_schedule{{0, 1, std::nullopt},
                                        {1, 0, std::nullopt}};
    const model::Schedule mapped_race_schedule =
        swap_label_shifted_two_thread_schedule(race_schedule);
    const model::CheckResult race = race_checker.replay(race_schedule);
    model::CheckResult mapped_race = race_checker.replay(mapped_race_schedule);
    require(race.first_race.has_value() && mapped_race.first_race.has_value(),
            "label-shifted writer schedules must produce symmetric races");
    require(symmetry_diagnosis::report_payloads_are_isomorphic(
                race_program, race, mapped_race, swap),
            "race report endpoints and embedded schedule should translate");
    mapped_race.first_race->schedule.pop_back();
    require(!symmetry_diagnosis::report_payloads_are_isomorphic(
                race_program, race, mapped_race, swap),
            "a mismatched embedded race schedule must fail isomorphism");

    const auto race_measurement = symmetry_diagnosis::measure_program(
        race_program,
        race_checker,
        model::MemoryModel::SC,
        race_checker.explore_naive(),
        race_checker.explore_dpor(),
        100);
    require(race_measurement.orbit.has_value(),
            "the label-shifted race fixture should be orbit eligible");

    const Program deadlock_program{{
        {label("before"), lock("m"), label("between"), lock("m")},
        {lock("m"), label("between"), lock("m"), label("after")},
    }};
    const model::ModelChecker deadlock_checker(deadlock_program);
    const model::CheckResult deadlock_naive = deadlock_checker.explore_naive();
    const std::vector<model::Schedule> deadlock_schedules =
        deadlock_checker.collect_naive_schedules(
            deadlock_naive.schedules_explored + 1);
    require(!deadlock_schedules.empty(),
            "label-shifted double-lock fixture needs a deadlock witness");
    const model::Schedule& deadlock_schedule = deadlock_schedules.front();
    const model::Schedule mapped_deadlock_schedule =
        swap_label_shifted_two_thread_schedule(deadlock_schedule);
    const model::CheckResult deadlock =
        deadlock_checker.replay(deadlock_schedule);
    model::CheckResult mapped_deadlock =
        deadlock_checker.replay(mapped_deadlock_schedule);
    require(deadlock.first_deadlock.has_value() &&
                mapped_deadlock.first_deadlock.has_value(),
            "label-shifted double locks must produce symmetric deadlocks");
    require(symmetry_diagnosis::report_payloads_are_isomorphic(
                deadlock_program, deadlock, mapped_deadlock, swap),
            "deadlock blockers and embedded schedule should translate");
    mapped_deadlock.first_deadlock->schedule.pop_back();
    require(!symmetry_diagnosis::report_payloads_are_isomorphic(
                deadlock_program, deadlock, mapped_deadlock, swap),
            "a mismatched embedded deadlock schedule must fail isomorphism");

    const model::CheckResult deadlock_dpor = deadlock_checker.explore_dpor();
    const auto deadlock_measurement = symmetry_diagnosis::measure_program(
        deadlock_program,
        deadlock_checker,
        model::MemoryModel::SC,
        deadlock_naive,
        deadlock_dpor,
        100);
    require(deadlock_measurement.orbit.has_value(),
            "the label-shifted deadlock fixture should be orbit eligible");
}

void outcome_signatures_cover_every_in_scope_report_combination() {
    model::CheckResult result;
    require(symmetry_diagnosis::outcome_signature(result) ==
                symmetry_diagnosis::OutcomeSignature::Clean,
            "no report should classify as clean");
    result.first_race = model::RaceReport{};
    require(symmetry_diagnosis::outcome_signature(result) ==
                symmetry_diagnosis::OutcomeSignature::Race,
            "a race-only result should retain its race bit");
    result.first_deadlock = model::DeadlockReport{};
    require(symmetry_diagnosis::outcome_signature(result) ==
                symmetry_diagnosis::OutcomeSignature::RaceAndDeadlock,
            "simultaneous race and deadlock reports need a distinct signature");
    result.first_race.reset();
    require(symmetry_diagnosis::outcome_signature(result) ==
                symmetry_diagnosis::OutcomeSignature::Deadlock,
            "a deadlock-only result should retain its deadlock bit");
    require(std::string(symmetry_diagnosis::exclusion_name(
                symmetry_diagnosis::OrbitExclusion::ClassOutcomeViolation)) ==
                "class-outcome-violation",
            "class outcome inconsistencies need a stable exclusion label");
}

void pso_nonzero_flush_address_survives_worker_permutation() {
    const Program program{{
        {write("z")},
        {write("z")},
        {write("a")},
    }};
    const model::ModelChecker checker(
        program,
        model::ModelChecker::kDefaultStepBound,
        model::MemoryModel::PSO);
    const model::CheckResult naive = checker.explore_naive();
    const model::CheckResult dpor = checker.explore_dpor();
    bool saw_worker_flush_one = false;
    bool saw_fixed_flush_zero = false;
    for (const model::Schedule& schedule : checker.collect_naive_schedules(
             naive.schedules_explored + 1)) {
        for (const model::ScheduleStep& step : schedule) {
            saw_worker_flush_one = saw_worker_flush_one ||
                (step.thread < 2 &&
                 step.action_index == model::kFlushActionIndex &&
                 step.flush_address == 1);
            saw_fixed_flush_zero = saw_fixed_flush_zero ||
                (step.thread == 2 &&
                 step.action_index == model::kFlushActionIndex &&
                 step.flush_address == 0);
        }
    }
    require(saw_worker_flush_one && saw_fixed_flush_zero,
            "PSO fixture must exercise nonzero worker and zero fixed flush ids");

    const auto measurement = symmetry_diagnosis::measure_program(
        program,
        checker,
        model::MemoryModel::PSO,
        naive,
        dpor,
        500);
    require(measurement.orbit.has_value(),
            "PSO worker swap with address id one should remain orbit eligible");
    require(measurement.orbit->mazurkiewicz_classes == 2 &&
                measurement.orbit->orbit_classes == 1,
            "two dependent z-flush orders should form two classes and one orbit");
}

void already_optimal_three_reader_fixture_has_no_symmetry_headroom() {
    const Program program{{
        {rlock("rw"), read("x"), runlock("rw")},
        {rlock("rw"), read("x"), runlock("rw")},
        {rlock("rw"), read("x"), runlock("rw")},
    }};
    const model::ModelChecker checker(program);
    const model::CheckResult naive = checker.explore_naive();
    const model::CheckResult dpor = checker.explore_dpor();
    const auto measurement = symmetry_diagnosis::measure_program(
        program,
        checker,
        model::MemoryModel::SC,
        naive,
        dpor,
        2000);
    require(naive.schedules_explored == 1680,
            "three-reader fixture naive baseline changed");
    require(dpor.schedules_explored == 1,
            "three-reader fixture DPOR baseline changed");
    require(measurement.orbit.has_value() &&
                measurement.orbit->mazurkiewicz_classes == 1 &&
                measurement.orbit->orbit_classes == 1,
            "thread permutation cannot improve an already single-class fixture");
}

void corpus_recorder_separates_candidates_automorphisms_and_orbit_savings() {
    symmetry_diagnosis::reset_corpora();

    const Program writes{{{write("x")}, {write("x")}}};
    const model::ModelChecker writes_checker(writes);
    const model::CheckResult writes_naive = writes_checker.explore_naive();
    const model::CheckResult writes_dpor = writes_checker.explore_dpor();
    symmetry_diagnosis::record_program(
        "unit",
        {"dependent-writes", "two-threads"},
        writes,
        writes_checker,
        model::MemoryModel::SC,
        writes_naive,
        writes_dpor,
        100,
        {1000, 500});

    const Program targeted{{{yield()}, {yield()}, {join(0)}}};
    const model::ModelChecker targeted_checker(targeted);
    const model::CheckResult targeted_naive = targeted_checker.explore_naive();
    const model::CheckResult targeted_dpor = targeted_checker.explore_dpor();
    symmetry_diagnosis::record_program(
        "unit",
        {"targeted", "three-threads"},
        targeted,
        targeted_checker,
        model::MemoryModel::SC,
        targeted_naive,
        targeted_dpor,
        100,
        {2000, 750});

    const std::vector<std::string> summaries =
        symmetry_diagnosis::summary_lines();
    require(summaries.size() == 2,
            "one deterministic count line and one timing line are expected");
    require(summaries.at(0).find(
                "programs=2 identical_body_programs=2 automorphic_programs=1") !=
                std::string::npos,
            "summary must distinguish syntactic candidates from automorphisms");
    require(summaries.at(0).find(
                "orbit_eligible_programs=1 orbit_naive_schedules=2 "
                "orbit_dpor_schedules=2 "
                "mazurkiewicz_classes=2 orbit_classes=1") !=
                std::string::npos,
            "summary must retain raw classes, orbit classes, and current DPOR: " +
                summaries.at(0));
    require(summaries.at(0).find(
                "excluded_class_outcome_violation=0") != std::string::npos,
            "summary must publish the per-class outcome consistency exclusion");
    require(summaries.at(1).find(
                "estimated_gross_saved_ns=250") != std::string::npos,
            "timing estimate must scale DPOR time by the measured D-to-O gap");
}

} // namespace

int main() {
    label_names_and_positions_are_normalized();
    external_thread_references_can_break_body_candidate_symmetry();
    a_fixed_external_join_target_preserves_the_worker_swap();
    lowest_waiter_signal_priority_blocks_thread_swaps();
    resource_renaming_is_outside_exact_body_symmetry();
    dependent_equal_writes_have_one_orbit_but_two_trace_classes();
    report_schedules_are_checked_in_original_raw_coordinates();
    outcome_signatures_cover_every_in_scope_report_combination();
    pso_nonzero_flush_address_survives_worker_permutation();
    already_optimal_three_reader_fixture_has_no_symmetry_headroom();
    corpus_recorder_separates_candidates_automorphisms_and_orbit_savings();
    return 0;
}
