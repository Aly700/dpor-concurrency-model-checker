#include "model/checker.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

model::Action barrier_wait(std::string barrier, std::uint32_t parties) {
    model::Action action;
    action.kind = model::ActionKind::BarrierWait;
    action.barrier = std::move(barrier);
    action.parties = parties;
    return action;
}

model::Action yield_action() {
    model::Action action;
    action.kind = model::ActionKind::Yield;
    return action;
}

model::Action write(std::string address) {
    model::Action action;
    action.kind = model::ActionKind::Write;
    action.address = std::move(address);
    return action;
}

model::Action read(std::string address) {
    model::Action action;
    action.kind = model::ActionKind::Read;
    action.address = std::move(address);
    return action;
}

model::Action lock(std::string mutex) {
    model::Action action;
    action.kind = model::ActionKind::Lock;
    action.mutex = std::move(mutex);
    return action;
}

model::Action rlock(std::string rwlock) {
    model::Action action;
    action.kind = model::ActionKind::RLock;
    action.rwlock = std::move(rwlock);
    return action;
}

model::Action sem_post(std::string semaphore) {
    model::Action action;
    action.kind = model::ActionKind::SemPost;
    action.semaphore = std::move(semaphore);
    return action;
}

model::Action signal(std::string condition) {
    model::Action action;
    action.kind = model::ActionKind::Signal;
    action.condition = std::move(condition);
    return action;
}

model::Action set_one() {
    model::Action action;
    action.kind = model::ActionKind::Set;
    action.destination = 0;
    action.value = model::ValueOperand{
        model::ValueOperandKind::Immediate,
        1,
        0,
    };
    return action;
}

model::Action label(std::string name) {
    model::Action action;
    action.kind = model::ActionKind::Label;
    action.label = std::move(name);
    return action;
}

model::Action branch_to(std::string target) {
    model::Action action;
    action.kind = model::ActionKind::BranchNonzero;
    action.source_register = 0;
    action.label = std::move(target);
    return action;
}

model::ScheduleStep step(model::ThreadId thread, std::uint32_t action_index) {
    return model::ScheduleStep{thread, action_index, std::nullopt};
}

model::ScheduleStep flush_step(model::ThreadId thread,
                               model::MemoryModel memory_model) {
    return model::ScheduleStep{
        thread,
        model::kFlushActionIndex,
        memory_model == model::MemoryModel::PSO
            ? std::optional<std::uint32_t>{0}
            : std::nullopt,
    };
}

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void require_clean(const model::CheckResult& result, const char* message) {
    require(!result.first_race.has_value(), message);
    require(!result.first_deadlock.has_value(), message);
    require(!result.first_error.has_value(), message);
    require(!result.first_assertion.has_value(), message);
    require(!result.first_nontermination.has_value(), message);
    require(result.bound_exceeded_executions == 0, message);
    require(result.cycles_detected == 0, message);
}

void require_replays_reports(const model::ModelChecker& checker,
                             const model::CheckResult& result) {
    if (result.first_deadlock.has_value()) {
        const auto replay = checker.replay(result.first_deadlock->schedule);
        require(replay.first_deadlock.has_value() &&
                    *replay.first_deadlock == *result.first_deadlock,
                "barrier deadlock report did not replay identically");
    }
    if (result.first_error.has_value()) {
        const auto replay = checker.replay(result.first_error->schedule);
        require(replay.first_error.has_value() &&
                    *replay.first_error == *result.first_error,
                "barrier modeled-error report did not replay identically");
    }
    if (result.first_nontermination.has_value()) {
        const auto replay = checker.replay(result.first_nontermination->schedule);
        require(replay.first_nontermination.has_value() &&
                    *replay.first_nontermination == *result.first_nontermination,
                "barrier lasso report did not replay identically");
    }
}

void require_naive_dpor_agree(const model::Program& program,
                              std::size_t step_bound =
                                  model::ModelChecker::kDefaultStepBound,
                              model::MemoryModel memory_model =
                                  model::MemoryModel::SC) {
    const model::ModelChecker checker(program, step_bound, memory_model);
    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    require(naive.first_race.has_value() == dpor.first_race.has_value(),
            "barrier naive/DPOR race existence differs");
    require(naive.first_deadlock.has_value() == dpor.first_deadlock.has_value(),
            "barrier naive/DPOR deadlock existence differs");
    require(naive.first_error.has_value() == dpor.first_error.has_value(),
            "barrier naive/DPOR error existence differs");
    require(naive.first_assertion.has_value() ==
                dpor.first_assertion.has_value(),
            "barrier naive/DPOR assertion existence differs");
    require((naive.cycles_detected > 0) == (dpor.cycles_detected > 0),
            "barrier naive/DPOR cycle existence differs");
    require((naive.bound_exceeded_executions > 0) ==
                (dpor.bound_exceeded_executions > 0),
            "barrier naive/DPOR bound existence differs");
    require(dpor.schedules_explored <= naive.schedules_explored,
            "barrier DPOR explored more schedules than naive");
    require_replays_reports(checker, naive);
    require_replays_reports(checker, dpor);
}

void one_party_barrier_releases_immediately() {
    const model::Program program{{{
        barrier_wait("phase", 1),
        yield_action(),
    }}};
    const model::ModelChecker checker(program);
    require_clean(checker.replay({step(0, 0), step(0, 1)}),
                  "parties=1 barrier did not release immediately");
    require_naive_dpor_agree(program);
}

void arrivals_block_until_last_and_barrier_resets() {
    const model::Program program{{
        {barrier_wait("phase", 2), yield_action(),
         barrier_wait("phase", 2), yield_action()},
        {barrier_wait("phase", 2), yield_action(),
         barrier_wait("phase", 2), yield_action()},
    }};
    const model::ModelChecker checker(program);

    bool rejected_repeat_arrival = false;
    try {
        (void)checker.replay({step(0, 0), step(0, 0)});
    } catch (const std::invalid_argument&) {
        rejected_repeat_arrival = true;
    }
    require(rejected_repeat_arrival,
            "parked barrier participant arrived twice in one generation");

    require_clean(checker.replay({
                      step(0, 0), step(1, 0),
                      step(1, 1), step(0, 1),
                      step(1, 2), step(0, 2),
                      step(0, 3), step(1, 3),
                  }),
                  "cyclic barrier did not reset for its second generation");
    require_naive_dpor_agree(program);
}

void incomplete_generation_deadlocks_with_barrier_tags() {
    const model::Program program{{
        {barrier_wait("phase", 3)},
        {barrier_wait("phase", 3)},
    }};
    const model::ModelChecker checker(program);
    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();

    for (const model::CheckResult* result : {&naive, &dpor}) {
        require(result->first_deadlock.has_value(),
                "undersubscribed barrier did not deadlock");
        require(result->first_deadlock->blocked_threads.size() == 2,
                "barrier deadlock omitted a parked participant");
        for (model::ThreadId tid = 0; tid < 2; ++tid) {
            const auto& blocked = result->first_deadlock->blocked_threads.at(tid);
            require(blocked.thread == tid &&
                        blocked.kind == model::BlockedOnKind::Barrier &&
                        blocked.barrier == "phase",
                    "barrier deadlock used the wrong blocker identity");
        }
        require_replays_reports(checker, *result);
    }
    require_naive_dpor_agree(program);
}

void invalid_party_counts_are_forward_modeled_errors() {
    const model::Program zero{{{barrier_wait("phase", 0)}}};
    const model::ModelChecker zero_checker(zero);
    const auto zero_result = zero_checker.explore_naive();
    require(zero_result.first_error.has_value(),
            "parties=0 barrier was not a modeled error");
    require(zero_result.first_error->endpoint == step(0, 0),
            "parties=0 error used the wrong endpoint");
    require(zero_result.first_error->message.find("parties") != std::string::npos,
            "parties=0 error omitted its cause");
    require_replays_reports(zero_checker, zero_result);
    require_naive_dpor_agree(zero);

    const model::Program mismatch{{
        {barrier_wait("phase", 2)},
        {barrier_wait("phase", 3)},
    }};
    const model::ModelChecker mismatch_checker(mismatch);
    const auto mismatch_result = mismatch_checker.explore_naive();
    require(mismatch_result.first_error.has_value(),
            "program-wide party disagreement was not a modeled error");
    require(mismatch_result.first_error->endpoint == step(1, 0),
            "party disagreement did not name the mismatching action");
    require(mismatch_result.first_error->message.find("requires 2") !=
                std::string::npos,
            "party disagreement error omitted the canonical party count");
    require_replays_reports(mismatch_checker, mismatch_result);
    require_naive_dpor_agree(mismatch);
}

void barrier_namespace_is_distinct_from_every_sync_namespace() {
    const std::vector<model::Program> invalid_programs = {
        model::Program{{{barrier_wait("phase", 1)}, {lock("phase")}}},
        model::Program{{{rlock("phase")}, {barrier_wait("phase", 1)}}},
        model::Program{{{barrier_wait("phase", 1)}, {sem_post("phase")}}},
        model::Program{{{signal("phase")}, {barrier_wait("phase", 1)}}},
    };
    for (const model::Program& program : invalid_programs) {
        bool threw = false;
        try {
            (void)model::ModelChecker(program);
        } catch (const std::invalid_argument& error) {
            threw = std::string(error.what()).find("barrier") !=
                    std::string::npos;
        }
        require(threw,
                "ModelChecker accepted a barrier name from another sync namespace");
    }
}

void buffered_barriers_wait_for_store_buffers_to_drain() {
    for (const model::MemoryModel memory_model : {model::MemoryModel::TSO,
                                                  model::MemoryModel::PSO}) {
        const model::Program program{{{
            write("x"),
            barrier_wait("phase", 1),
        }}};
        const model::ModelChecker checker(program, 20, memory_model);
        bool rejected = false;
        try {
            (void)checker.replay({step(0, 0), step(0, 1)});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected,
                "BarrierWait executed before its store buffer drained");
        require_clean(checker.replay({
                          step(0, 0),
                          flush_step(0, memory_model),
                          step(0, 1),
                      }),
                      "BarrierWait did not execute after its store buffer drained");
        require_naive_dpor_agree(program, 20, memory_model);
    }
}

void barrier_arrivals_are_behavioral_but_generation_clocks_are_not() {
    const model::Program undersubscribed_loop{{
        {set_one(), label("again"), barrier_wait("phase", 3),
         branch_to("again")},
        {set_one(), label("again"), barrier_wait("phase", 3),
         branch_to("again")},
    }};
    const model::ModelChecker undersubscribed_checker(undersubscribed_loop, 8);
    const auto undersubscribed = undersubscribed_checker.explore_naive();
    require(undersubscribed.first_deadlock.has_value(),
            "undersubscribed cyclic barrier did not deadlock");
    require(undersubscribed.cycles_detected == 0,
            "barrier arrivals were omitted from behavioral-state equality");

    const model::Program balanced_loop{{
        {set_one(), label("again"), barrier_wait("phase", 2),
         branch_to("again")},
        {set_one(), label("again"), barrier_wait("phase", 2),
         branch_to("again")},
    }};
    const model::ModelChecker balanced_checker(balanced_loop, 10);
    const auto balanced = balanced_checker.explore_naive();
    require(balanced.cycles_detected > 0 &&
                balanced.first_nontermination.has_value(),
            "barrier generation ordinal or release clock leaked into behavioral equality");
    require_replays_reports(balanced_checker, balanced);
    require_naive_dpor_agree(balanced_loop, 10);
}

void public_independence_stays_conservative_without_arrival_state() {
    require(!model::independent(barrier_wait("phase", 3),
                                barrier_wait("phase", 3)),
            "action-only independence guessed at same-barrier arrival state");
    require(model::independent(barrier_wait("left", 2),
                               barrier_wait("right", 2)),
            "different barriers were not independent");
}

void low_count_arrivals_commute_but_last_arrival_remains_dependent() {
    const model::Program program{{
        {barrier_wait("phase", 3)},
        {barrier_wait("phase", 3)},
        {barrier_wait("phase", 3)},
    }};
    const model::ModelChecker checker(program);
    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    require_clean(naive, "three-party discriminator was not clean under naive");
    require_clean(dpor, "three-party discriminator was not clean under DPOR");
    require(naive.schedules_explored == 6,
            "three-party discriminator did not have all six arrival orders");
    require(dpor.schedules_explored == 3,
            "last-arrival-aware DPOR did not keep exactly one class per last arriver");
}

void persistent_set_keeps_the_alternative_generation_membership_witness() {
    const model::Program program{{
        {write("x"), barrier_wait("phase", 3)},
        {barrier_wait("phase", 3), read("x")},
        {barrier_wait("phase", 3)},
        {barrier_wait("phase", 3)},
    }};
    const model::ModelChecker checker(program);
    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    require(naive.first_race.has_value(),
            "generation-membership witness did not contain its expected race");
    require(dpor.first_race.has_value(),
            "barrier persistent set pruned the alternative membership race");
    require(dpor.schedules_explored <= naive.schedules_explored,
            "barrier membership witness violated schedule dominance");
    require_replays_reports(checker, naive);
    require_replays_reports(checker, dpor);
}

void repeated_source_endpoint_is_distinguished_by_barrier_generation() {
    const model::Program program{{
        {set_one(), label("again"), barrier_wait("phase", 3),
         branch_to("again")},
        {set_one(), label("again"), barrier_wait("phase", 3),
         branch_to("again")},
        {set_one(), label("again"), barrier_wait("phase", 3),
         branch_to("again")},
    }};
    const model::ModelChecker checker(program, 12);
    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    require(naive.cycles_detected > 0 &&
                naive.first_nontermination.has_value(),
            "naive exploration missed the cyclic three-party barrier lasso");
    require(dpor.cycles_detected > 0 &&
                dpor.first_nontermination.has_value(),
            "DPOR confused repeated barrier endpoints across generations");
    require(dpor.schedules_explored <= naive.schedules_explored,
            "generation-stamped barrier loop violated schedule dominance");
    require_replays_reports(checker, naive);
    require_replays_reports(checker, dpor);
    require_naive_dpor_agree(program, 12);
}

} // namespace

int main() {
    one_party_barrier_releases_immediately();
    arrivals_block_until_last_and_barrier_resets();
    incomplete_generation_deadlocks_with_barrier_tags();
    invalid_party_counts_are_forward_modeled_errors();
    barrier_namespace_is_distinct_from_every_sync_namespace();
    buffered_barriers_wait_for_store_buffers_to_drain();
    barrier_arrivals_are_behavioral_but_generation_clocks_are_not();
    public_independence_stays_conservative_without_arrival_state();
    low_count_arrivals_commute_but_last_arrival_remains_dependent();
    persistent_set_keeps_the_alternative_generation_membership_witness();
    repeated_source_endpoint_is_distinguished_by_barrier_generation();
    return 0;
}
