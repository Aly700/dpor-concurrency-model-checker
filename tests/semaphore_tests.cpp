#include "model/checker.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

model::Action action_with_address(model::ActionKind kind, std::string address) {
    model::Action action;
    action.kind = kind;
    action.address = std::move(address);
    return action;
}

model::Action read(std::string address) {
    return action_with_address(model::ActionKind::Read, std::move(address));
}

model::Action write(std::string address) {
    return action_with_address(model::ActionKind::Write, std::move(address));
}

model::Action semaphore_action(model::ActionKind kind, std::string semaphore) {
    model::Action action;
    action.kind = kind;
    action.semaphore = std::move(semaphore);
    return action;
}

model::Action sem_post(std::string semaphore) {
    return semaphore_action(model::ActionKind::SemPost, std::move(semaphore));
}

model::Action sem_wait(std::string semaphore) {
    return semaphore_action(model::ActionKind::SemWait, std::move(semaphore));
}

model::Action mutex_lock(std::string mutex) {
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

model::Action set_register(model::RegisterId reg, model::Value value) {
    model::Action action;
    action.kind = model::ActionKind::Set;
    action.destination = reg;
    action.value = model::ValueOperand{model::ValueOperandKind::Immediate, value, 0};
    return action;
}

model::Action label(std::string name) {
    model::Action action;
    action.kind = model::ActionKind::Label;
    action.label = std::move(name);
    return action;
}

model::Action branch_nonzero(model::RegisterId reg, std::string target) {
    model::Action action;
    action.kind = model::ActionKind::BranchNonzero;
    action.source_register = reg;
    action.label = std::move(target);
    return action;
}

model::ScheduleStep step(model::ThreadId thread, std::uint32_t action_index) {
    return model::ScheduleStep{thread, action_index, std::nullopt};
}

model::ScheduleStep flush_step(model::ThreadId thread, model::MemoryModel memory_model) {
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
    if (result.first_race.has_value()) {
        const auto replay = checker.replay(result.first_race->schedule);
        require(replay.first_race.has_value() &&
                    *replay.first_race == *result.first_race,
                "semaphore race report did not replay identically");
    }
    if (result.first_deadlock.has_value()) {
        const auto replay = checker.replay(result.first_deadlock->schedule);
        require(replay.first_deadlock.has_value() &&
                    *replay.first_deadlock == *result.first_deadlock,
                "semaphore deadlock report did not replay identically");
    }
}

void require_naive_dpor_agree(const model::Program& program,
                              std::size_t step_bound = model::ModelChecker::kDefaultStepBound,
                              model::MemoryModel memory_model = model::MemoryModel::SC) {
    const model::ModelChecker checker(program, step_bound, memory_model);
    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    require(naive.first_race.has_value() == dpor.first_race.has_value(),
            "semaphore naive/DPOR race existence differs");
    require(naive.first_deadlock.has_value() == dpor.first_deadlock.has_value(),
            "semaphore naive/DPOR deadlock existence differs");
    require(naive.first_error.has_value() == dpor.first_error.has_value(),
            "semaphore naive/DPOR error existence differs");
    require(naive.first_assertion.has_value() == dpor.first_assertion.has_value(),
            "semaphore naive/DPOR assertion existence differs");
    require((naive.cycles_detected > 0) == (dpor.cycles_detected > 0),
            "semaphore naive/DPOR cycle existence differs");
    require((naive.fair_cycles > 0) == (dpor.fair_cycles > 0),
            "semaphore naive/DPOR fair-cycle existence differs");
    require((naive.strongly_unfair_cycles > 0) ==
                (dpor.strongly_unfair_cycles > 0),
            "semaphore naive/DPOR strongly-unfair-cycle existence differs");
    require((naive.unfair_cycles > 0) == (dpor.unfair_cycles > 0),
            "semaphore naive/DPOR unfair-cycle existence differs");
    require((naive.bound_exceeded_executions > 0) ==
                (dpor.bound_exceeded_executions > 0),
            "semaphore naive/DPOR bound existence differs");
    require(dpor.schedules_explored <= naive.schedules_explored,
            "semaphore DPOR explored more schedules than naive");
    require_replays_reports(checker, naive);
    require_replays_reports(checker, dpor);
}

void zero_initial_permits_deadlock_with_a_semaphore_blocker() {
    const model::Program program{{{sem_wait("gate")}}};
    const model::ModelChecker checker(program);
    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();

    for (const model::CheckResult* result : {&naive, &dpor}) {
        require(result->first_deadlock.has_value(),
                "zero-initialized semaphore wait did not deadlock");
        require(result->first_deadlock->schedule.empty(),
                "initial semaphore deadlock acquired a phantom permit");
        require(result->first_deadlock->blocked_threads.size() == 1,
                "semaphore deadlock omitted its blocked thread");
        const model::BlockedThread& blocked =
            result->first_deadlock->blocked_threads.front();
        require(blocked.thread == 0 &&
                    blocked.kind == model::BlockedOnKind::Semaphore &&
                    blocked.semaphore == "gate",
                "semaphore deadlock used the wrong blocker taxonomy");
        require_replays_reports(checker, *result);
    }
    require_naive_dpor_agree(program);
}

void explicit_posts_seed_and_waits_consume_exactly_one_permit() {
    const model::Program balanced{{{
        sem_post("gate"),
        sem_post("gate"),
        sem_wait("gate"),
        sem_wait("gate"),
    }}};
    const model::ModelChecker balanced_checker(balanced);
    require_clean(balanced_checker.explore_naive(),
                  "two explicit posts did not seed two permits");
    require_clean(balanced_checker.explore_dpor(),
                  "DPOR changed balanced semaphore permit accounting");

    const model::Program exhausted{{{
        sem_post("gate"),
        sem_post("gate"),
        sem_wait("gate"),
        sem_wait("gate"),
        sem_wait("gate"),
    }}};
    const model::ModelChecker exhausted_checker(exhausted);
    const auto replay = exhausted_checker.replay(
        {step(0, 0), step(0, 1), step(0, 2), step(0, 3)});
    require(replay.first_deadlock.has_value(),
            "a third wait consumed a nonexistent semaphore permit");
    require(replay.first_deadlock->blocked_threads.size() == 1 &&
                replay.first_deadlock->blocked_threads.front().semaphore == "gate",
            "exhausted semaphore wait reported the wrong blocker");
    require_naive_dpor_agree(exhausted);
}

void post_release_to_wait_acquire_orders_publication() {
    const model::Program program{{
        {write("payload"), sem_post("ready")},
        {sem_wait("ready"), read("payload")},
    }};
    const model::ModelChecker checker(program);
    const auto replay = checker.replay(
        {step(0, 0), step(0, 1), step(1, 0), step(1, 1)});
    require_clean(replay,
                  "SemWait did not acquire the accumulated SemPost release clock");
    require_naive_dpor_agree(program);
}

void strong_accumulator_orders_after_every_prior_post_and_is_never_cleared() {
    const model::Program program{{
        {write("x"), sem_post("ready")},
        {write("y"), sem_post("ready")},
        {sem_wait("ready"), read("x"), read("y")},
        {sem_wait("ready"), read("x"), read("y")},
    }};
    const model::ModelChecker checker(program);
    const auto replay = checker.replay({
        step(0, 0), step(0, 1),
        step(1, 0), step(1, 1),
        step(2, 0), step(2, 1), step(2, 2),
        step(3, 0), step(3, 1), step(3, 2),
    });
    require_clean(replay,
                  "semaphore accumulator was replaced, cleared, or incompletely acquired");
}

void posts_do_not_acquire_prior_posters() {
    const model::Program program{{
        {write("x"), sem_post("ready")},
        {sem_post("ready"), write("x")},
    }};
    const model::ModelChecker checker(program);
    const auto replay = checker.replay(
        {step(0, 0), step(0, 1), step(1, 0), step(1, 1)});
    require(replay.first_race.has_value(),
            "SemPost acquired a prior poster clock and hid a poster/poster race");
    require(replay.first_race->address == "x",
            "poster/poster negative HB probe raced on the wrong address");
    require_replays_reports(checker, replay);
    require_naive_dpor_agree(program);
}

void buffered_semaphore_actions_wait_for_their_store_buffer_to_drain() {
    for (const model::MemoryModel memory_model : {model::MemoryModel::TSO,
                                                  model::MemoryModel::PSO}) {
        const model::Program posting{{{
            write("x"),
            sem_post("gate"),
        }}};
        const model::ModelChecker posting_checker(posting, 20, memory_model);
        bool rejected_post = false;
        try {
            (void)posting_checker.replay({step(0, 0), step(0, 1)});
        } catch (const std::invalid_argument&) {
            rejected_post = true;
        }
        require(rejected_post,
                "SemPost executed before the posting thread's buffer drained");
        require_clean(posting_checker.replay(
                          {step(0, 0), flush_step(0, memory_model), step(0, 1)}),
                      "SemPost did not execute after the posting thread drained");

        const model::Program waiting{{
            {write("x"), sem_wait("gate")},
            {sem_post("gate")},
        }};
        const model::ModelChecker waiting_checker(waiting, 20, memory_model);
        bool rejected_wait = false;
        try {
            (void)waiting_checker.replay(
                {step(1, 0), step(0, 0), step(0, 1)});
        } catch (const std::invalid_argument&) {
            rejected_wait = true;
        }
        require(rejected_wait,
                "SemWait executed before the waiting thread's buffer drained");
        require_clean(waiting_checker.replay({
                          step(1, 0),
                          step(0, 0),
                          flush_step(0, memory_model),
                          step(0, 1),
                      }),
                      "SemWait did not execute after the waiting thread drained");
    }
}

void semaphore_names_cannot_mix_with_mutex_or_rwlock_names() {
    const std::vector<model::Program> invalid_programs = {
        model::Program{{{mutex_lock("gate")}, {sem_post("gate")}}},
        model::Program{{{rlock("gate")}, {sem_wait("gate")}}},
    };
    for (const model::Program& program : invalid_programs) {
        bool threw = false;
        try {
            (void)model::ModelChecker(program);
        } catch (const std::invalid_argument& error) {
            const std::string message = error.what();
            threw = message.find("semaphore") != std::string::npos &&
                    (message.find("mutex") != std::string::npos ||
                     message.find("rwlock") != std::string::npos);
        }
        require(threw,
                "ModelChecker accepted a semaphore name from a lock namespace");
    }
}

void independence_matches_the_proved_semaphore_scope() {
    require(model::independent(sem_post("gate"), sem_post("gate")),
            "same-semaphore SemPost/SemPost commuting diamond was not applied");
    require(!model::independent(sem_post("gate"), sem_wait("gate")),
            "same-semaphore post/wait actions were incorrectly independent");
    require(!model::independent(sem_wait("gate"), sem_wait("gate")),
            "same-semaphore wait/wait actions were incorrectly independent");
    require(model::independent(sem_post("left"), sem_wait("right")),
            "different-semaphore actions were not independent");
}

void post_post_commutation_discriminator_has_exact_counts() {
    const model::Program program{{
        {sem_post("gate"), sem_wait("gate")},
        {sem_post("gate")},
    }};
    const model::ModelChecker checker(program);
    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    require(naive.schedules_explored == 3,
            "semaphore discriminator naive count changed from three legal orders");
    require(dpor.schedules_explored == 2,
            "SemPost/SemPost commutation did not retain exactly two DPOR representatives");
    require_clean(naive, "semaphore naive discriminator reported a bug");
    require_clean(dpor, "semaphore DPOR discriminator reported a bug");
}

void alternate_poster_middle_wait_race_is_not_pruned() {
    // The low-id poster can publish x before its post, producing a clean
    // waiter read. If the other poster enables the waiter first, the waiter
    // may read x before that publication and race with the later write. This
    // pins the disabled-wait all-enabled repair required when Posts commute.
    const model::Program program{{
        {write("x"), sem_post("gate")},
        {sem_post("gate")},
        {sem_wait("gate"), read("x")},
    }};
    const model::ModelChecker checker(program);
    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    require(naive.first_race.has_value(),
            "naive oracle missed the alternate-poster middle-wait race");
    require(dpor.first_race.has_value(),
            "SemPost commutation pruned the alternate-poster middle-wait race");
    require(naive.first_race->address == "x" && dpor.first_race->address == "x",
            "middle-wait repair probe raced on the wrong address");
    require_replays_reports(checker, naive);
    require_replays_reports(checker, dpor);
    require_naive_dpor_agree(program);
}

void permit_counts_are_behavioral_but_release_clocks_are_not() {
    const model::Program growing_count{{{
        set_register(0, 1),
        label("again"),
        sem_post("gate"),
        branch_nonzero(0, "again"),
    }}};
    const model::ModelChecker growing_checker(growing_count, 5);
    const auto growing = growing_checker.explore_naive();
    require(growing.cycles_detected == 0,
            "growing semaphore permit count was omitted from behavioral state");
    require(growing.bound_exceeded_executions > 0,
            "growing semaphore permit count did not reach the bound backstop");

    const model::Program balanced_cycle{{{
        set_register(0, 1),
        label("again"),
        sem_post("gate"),
        sem_wait("gate"),
        branch_nonzero(0, "again"),
    }}};
    const model::ModelChecker balanced_checker(balanced_cycle, 8);
    const auto balanced = balanced_checker.explore_naive();
    require(balanced.cycles_detected > 0 &&
                balanced.first_nontermination.has_value(),
            "semaphore HB accumulator leaked into behavioral-state equality");
}

} // namespace

int main() {
    zero_initial_permits_deadlock_with_a_semaphore_blocker();
    explicit_posts_seed_and_waits_consume_exactly_one_permit();
    post_release_to_wait_acquire_orders_publication();
    strong_accumulator_orders_after_every_prior_post_and_is_never_cleared();
    posts_do_not_acquire_prior_posters();
    buffered_semaphore_actions_wait_for_their_store_buffer_to_drain();
    semaphore_names_cannot_mix_with_mutex_or_rwlock_names();
    independence_matches_the_proved_semaphore_scope();
    post_post_commutation_discriminator_has_exact_counts();
    alternate_poster_middle_wait_race_is_not_pruned();
    permit_counts_are_behavioral_but_release_clocks_are_not();
    return 0;
}
