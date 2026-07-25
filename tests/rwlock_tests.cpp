#include "model/checker.hpp"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

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

model::Action mutex_lock(std::string mutex) {
    model::Action action;
    action.kind = model::ActionKind::Lock;
    action.mutex = std::move(mutex);
    return action;
}

model::Action rwlock_action(model::ActionKind kind, std::string rwlock) {
    model::Action action;
    action.kind = kind;
    action.rwlock = std::move(rwlock);
    return action;
}

model::Action rlock(std::string rwlock) {
    return rwlock_action(model::ActionKind::RLock, std::move(rwlock));
}

model::Action runlock(std::string rwlock) {
    return rwlock_action(model::ActionKind::RUnlock, std::move(rwlock));
}

model::Action wlock(std::string rwlock) {
    return rwlock_action(model::ActionKind::WLock, std::move(rwlock));
}

model::Action wunlock(std::string rwlock) {
    return rwlock_action(model::ActionKind::WUnlock, std::move(rwlock));
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
}

void require_replays_reports(const model::ModelChecker& checker,
                             const model::CheckResult& result) {
    if (result.first_race.has_value()) {
        const auto replay = checker.replay(result.first_race->schedule);
        require(replay.first_race.has_value() && *replay.first_race == *result.first_race,
                "rwlock race report did not replay identically");
    }
    if (result.first_deadlock.has_value()) {
        const auto replay = checker.replay(result.first_deadlock->schedule);
        require(replay.first_deadlock.has_value() &&
                    *replay.first_deadlock == *result.first_deadlock,
                "rwlock deadlock report did not replay identically");
    }
    if (result.first_error.has_value()) {
        const auto replay = checker.replay(result.first_error->schedule);
        require(replay.first_error.has_value() && *replay.first_error == *result.first_error,
                "rwlock modeled-error report did not replay identically");
    }
}

void require_naive_dpor_agree(const model::Program& program) {
    const model::ModelChecker checker(program);
    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    require(naive.first_race.has_value() == dpor.first_race.has_value(),
            "rwlock naive/DPOR race existence differs");
    require(naive.first_deadlock.has_value() == dpor.first_deadlock.has_value(),
            "rwlock naive/DPOR deadlock existence differs");
    require(naive.first_error.has_value() == dpor.first_error.has_value(),
            "rwlock naive/DPOR error existence differs");
    require(naive.first_assertion.has_value() == dpor.first_assertion.has_value(),
            "rwlock naive/DPOR assertion existence differs");
    require((naive.cycles_detected > 0) == (dpor.cycles_detected > 0),
            "rwlock naive/DPOR cycle existence differs");
    require((naive.fair_cycles > 0) == (dpor.fair_cycles > 0),
            "rwlock naive/DPOR fair-cycle existence differs");
    require((naive.strongly_unfair_cycles > 0) ==
                (dpor.strongly_unfair_cycles > 0),
            "rwlock naive/DPOR strongly-unfair-cycle existence differs");
    require((naive.unfair_cycles > 0) == (dpor.unfair_cycles > 0),
            "rwlock naive/DPOR unfair-cycle existence differs");
    require((naive.bound_exceeded_executions > 0) ==
                (dpor.bound_exceeded_executions > 0),
            "rwlock naive/DPOR bound existence differs");
    require(dpor.schedules_explored <= naive.schedules_explored,
            "rwlock DPOR explored more schedules than naive");
    require_replays_reports(checker, naive);
    require_replays_reports(checker, dpor);
}

bool has_rwlock_blocker(const model::DeadlockReport& report,
                        model::ThreadId thread,
                        model::BlockedOnKind kind,
                        const std::string& rwlock,
                        bool self_wait,
                        std::optional<model::ThreadId> owner = std::nullopt) {
    return std::any_of(
        report.blocked_threads.begin(),
        report.blocked_threads.end(),
        [&](const model::BlockedThread& blocked) {
            return blocked.thread == thread &&
                   blocked.kind == kind &&
                   blocked.rwlock == rwlock &&
                   blocked.self_wait == self_wait &&
                   blocked.owner == owner;
        });
}

void writer_release_to_reader_acquire_probe() {
    const model::Program program{{
        {wlock("rw"), write("x"), wunlock("rw")},
        {rlock("rw"), read("x"), runlock("rw")},
    }};
    const model::ModelChecker checker(program);
    const auto replay = checker.replay({{0, 0}, {0, 1}, {0, 2},
                                        {1, 0}, {1, 1}, {1, 2}});
    require_clean(replay, "RLock did not acquire the last writer-release clock");
}

void writer_release_to_writer_acquire_probe() {
    const model::Program program{{
        {wlock("rw"), write("x"), wunlock("rw")},
        {wlock("rw"), read("x"), wunlock("rw")},
    }};
    const model::ModelChecker checker(program);
    const auto replay = checker.replay({{0, 0}, {0, 1}, {0, 2},
                                        {1, 0}, {1, 1}, {1, 2}});
    require_clean(replay, "WLock did not acquire the prior writer-release clock");
}

void every_reader_release_to_writer_acquire_probe() {
    const model::Program program{{
        {rlock("rw"), write("x"), runlock("rw")},
        {rlock("rw"), write("y"), runlock("rw")},
        {wlock("rw"), read("x"), read("y"), wunlock("rw")},
    }};
    const model::ModelChecker checker(program);
    const auto replay = checker.replay({
        {0, 0}, {0, 1}, {0, 2},
        {1, 0}, {1, 1}, {1, 2},
        {2, 0}, {2, 1}, {2, 2}, {2, 3},
    });
    require_clean(replay, "WLock did not acquire every accumulated reader-release clock");
}

void readers_do_not_synchronize_with_each_other_probe() {
    const model::Program program{{
        {rlock("rw"), write("x"), runlock("rw")},
        {rlock("rw"), write("x"), runlock("rw")},
    }};
    const model::ModelChecker checker(program);
    const auto replay = checker.replay({{0, 0}, {0, 1}, {0, 2},
                                        {1, 0}, {1, 1}, {1, 2}});
    require(replay.first_race.has_value(),
            "reader-release was incorrectly joined by a later RLock and hid a race");
    require(replay.first_race->address == "x", "reader-reader HB probe raced on wrong address");
}

void parallel_readers_and_exclusive_writers_are_race_free() {
    const model::Program readers{{
        {wlock("rw"), write("x"), wunlock("rw")},
        {rlock("rw"), read("x"), runlock("rw")},
        {rlock("rw"), read("x"), runlock("rw")},
    }};
    const model::ModelChecker reader_checker(readers);
    require_clean(reader_checker.explore_naive(),
                  "parallel readers did not remain ordered against the writer");
    require_clean(reader_checker.explore_dpor(),
                  "DPOR reported a bug for correctly locked parallel readers");
    require_naive_dpor_agree(readers);

    const model::Program writers{{
        {wlock("rw"), write("x"), wunlock("rw")},
        {wlock("rw"), write("x"), wunlock("rw")},
    }};
    const model::ModelChecker writer_checker(writers);
    require_clean(writer_checker.explore_naive(), "writers were not mutually exclusive");
    require_clean(writer_checker.explore_dpor(), "DPOR broke writer exclusion");
    require_naive_dpor_agree(writers);
}

void unlocked_reader_races_with_writer() {
    const model::Program program{{
        {wlock("rw"), write("x"), wunlock("rw")},
        {read("x")},
    }};
    const model::ModelChecker checker(program);
    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    require(naive.first_race.has_value(), "unlocked reader lost its naive race");
    require(dpor.first_race.has_value(), "unlocked reader race was pruned by DPOR");
    require_naive_dpor_agree(program);
}

void non_holder_unlocks_are_modeled_errors() {
    for (const model::Action action : {runlock("rw"), wunlock("rw")}) {
        const model::Program program{{{action}}};
        const model::ModelChecker checker(program);
        const auto naive = checker.explore_naive();
        const auto dpor = checker.explore_dpor();
        require(naive.first_error.has_value(), "non-holder rwlock unlock was not an error");
        require(dpor.first_error.has_value(), "DPOR lost non-holder rwlock unlock error");
        require(naive.first_error->message.find("rwlock 'rw'") != std::string::npos,
                "rwlock unlock error omitted the rwlock name");
        require_replays_reports(checker, naive);
        require_replays_reports(checker, dpor);
    }

    const model::Program wrong_modes{{
        {rlock("read-held"), wunlock("read-held")},
        {wlock("write-held"), runlock("write-held")},
    }};
    const model::ModelChecker checker(wrong_modes);
    const auto result = checker.explore_naive();
    require(result.first_error.has_value(), "wrong-mode rwlock unlock was not an error");
    require_naive_dpor_agree(wrong_modes);
}

void reentrancy_is_a_modeled_error_but_upgrade_is_a_deadlock() {
    const std::initializer_list<model::Program> errors = {
        model::Program{{{rlock("rw"), rlock("rw")}}},
        model::Program{{{wlock("rw"), wlock("rw")}}},
        model::Program{{{wlock("rw"), rlock("rw")}}},
    };
    for (const model::Program& program : errors) {
        const model::ModelChecker checker(program);
        const auto result = checker.explore_naive();
        require(result.first_error.has_value(), "rwlock reentrancy was not a modeled error");
        require(result.first_error->endpoint.action_index == 1,
                "rwlock reentrancy error had the wrong endpoint");
        require_replays_reports(checker, result);
        require_naive_dpor_agree(program);
    }

    const model::Program upgrade{{{rlock("rw"), wlock("rw")}}};
    const model::ModelChecker checker(upgrade);
    const auto result = checker.explore_naive();
    require(!result.first_error.has_value(), "read-to-write upgrade incorrectly became an error");
    require(result.first_deadlock.has_value(), "read-to-write upgrade did not deadlock");
    require(has_rwlock_blocker(*result.first_deadlock,
                               0,
                               model::BlockedOnKind::RwLockReaders,
                               "rw",
                               true),
            "read-to-write upgrade deadlock omitted self_wait");
    require_replays_reports(checker, result);
    require_naive_dpor_agree(upgrade);
}

void deadlock_reports_distinguish_writer_and_reader_blockers() {
    const model::Program writer_held{{
        {wlock("rw")},
        {rlock("rw")},
    }};
    const model::ModelChecker writer_checker(writer_held);
    const auto writer_wait = writer_checker.replay({{0, 0}});
    require(writer_wait.first_deadlock.has_value(), "writer-held rwlock did not deadlock reader");
    require(has_rwlock_blocker(*writer_wait.first_deadlock,
                               1,
                               model::BlockedOnKind::RwLockWriter,
                               "rw",
                               false,
                               0),
            "writer-held deadlock used the wrong rwlock blocker tag");
    require_replays_reports(writer_checker, writer_wait);

    const model::Program reader_held{{
        {rlock("rw")},
        {wlock("rw")},
    }};
    const model::ModelChecker reader_checker(reader_held);
    const auto readers_wait = reader_checker.replay({{0, 0}});
    require(readers_wait.first_deadlock.has_value(), "reader-held rwlock did not deadlock writer");
    require(has_rwlock_blocker(*readers_wait.first_deadlock,
                               1,
                               model::BlockedOnKind::RwLockReaders,
                               "rw",
                               false),
            "readers-to-drain deadlock used the wrong rwlock blocker tag");
    require_replays_reports(reader_checker, readers_wait);

    require_naive_dpor_agree(writer_held);
    require_naive_dpor_agree(reader_held);
}

void buffered_rwlock_actions_wait_for_store_buffer_drain() {
    for (const model::MemoryModel memory_model : {model::MemoryModel::TSO,
                                                  model::MemoryModel::PSO}) {
        for (const bool writer_mode : {false, true}) {
            const model::Program program{{{
                write("x"),
                writer_mode ? wlock("rw") : rlock("rw"),
                writer_mode ? wunlock("rw") : runlock("rw"),
            }}};
            const model::ModelChecker checker(program, 20, memory_model);

            bool rejected_ordered_point = false;
            try {
                (void)checker.replay({{0, 0}, {0, 1}});
            } catch (const std::invalid_argument&) {
                rejected_ordered_point = true;
            }
            require(rejected_ordered_point,
                    "rwlock action executed before its buffered writes drained");

            model::Schedule schedule{{0, 0}};
            schedule.push_back(model::ScheduleStep{
                0,
                model::kFlushActionIndex,
                memory_model == model::MemoryModel::PSO
                    ? std::optional<std::uint32_t>{0}
                    : std::nullopt,
            });
            schedule.push_back({0, 1});
            schedule.push_back({0, 2});
            require_clean(checker.replay(schedule),
                          "rwlock action did not execute cleanly after buffer drain");
        }
    }
}

void mutex_and_rwlock_namespaces_cannot_mix() {
    const model::Program program{{
        {mutex_lock("gate")},
        {rlock("gate")},
    }};
    bool threw = false;
    try {
        (void)model::ModelChecker(program);
    } catch (const std::invalid_argument& error) {
        threw = std::string(error.what()).find("mutex") != std::string::npos &&
                std::string(error.what()).find("rwlock") != std::string::npos;
    }
    require(threw, "ModelChecker accepted a mixed mutex/rwlock namespace name");
}

void independence_clauses_match_the_proved_scope() {
    require(!model::independent(rlock("rw"), rlock("rw")),
            "public RLock/RLock bypassed the Upgrade-sensitive safeguard");
    require(model::independent(rlock("a"), wlock("b")),
            "different rwlock names were not independent");
    require(!model::independent(rlock("rw"), runlock("rw")),
            "public RLock/RUnlock unexpectedly bypassed the conservative baseline");
    require(!model::independent(runlock("rw"), runlock("rw")),
            "public RUnlock/RUnlock unexpectedly bypassed the conservative baseline");
    require(!model::independent(rlock("rw"), wlock("rw")),
            "same-rwlock reader/writer operations were independent");
    require(!model::independent(wunlock("rw"), rlock("rw")),
            "same-rwlock WUnlock/RLock operations were independent");

    const model::Program reader_only{{
        {rlock("rw"), runlock("rw")},
        {rlock("rw"), runlock("rw")},
    }};
    const model::ModelChecker reader_checker(reader_only);
    require(reader_checker.dpor_transitions_independent(0, rlock("rw"), 1, runlock("rw")),
            "writer-free RLock/RUnlock refinement was not applied");
    require(reader_checker.dpor_transitions_independent(0, runlock("rw"), 1, runlock("rw")),
            "writer-free RUnlock/RUnlock refinement was not applied");

    const model::Program writer_present{{
        {rlock("rw"), runlock("rw")},
        {wlock("rw"), wunlock("rw")},
    }};
    const model::ModelChecker writer_checker(writer_present);
    require(!writer_checker.dpor_transitions_independent(0, rlock("rw"), 1, runlock("rw")),
            "writer-bearing RLock/RUnlock ignored the middle-witness safeguard");
    require(!writer_checker.dpor_transitions_independent(0, runlock("rw"), 1, runlock("rw")),
            "writer-bearing RUnlock/RUnlock ignored the conservative safeguard");
}

void terminal_reentrant_reader_does_not_prune_enabled_siblings() {
    // The writer-free checker-local refinement commutes all reader-mode
    // operations. The first thread's recursive RLock is nevertheless a
    // terminal modeled error rather than a successful side of that diamond.
    // Pin the generic terminal safeguard: after first choosing the low-id
    // error endpoint, DPOR must still backtrack every enabled sibling far
    // enough to find the peer write/write race.
    const model::Program program{{
        {rlock("rw"), rlock("rw")},
        {rlock("rw"), write("x"), runlock("rw")},
        {write("x")},
    }};
    const model::ModelChecker checker(program);
    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    require(naive.first_error.has_value() && dpor.first_error.has_value(),
            "reentrant-reader terminal safeguard lost the modeled error");
    require(naive.first_race.has_value() && dpor.first_race.has_value(),
            "reentrant-reader terminal safeguard pruned an enabled-sibling race");
    require_replays_reports(checker, dpor);
}

void three_reader_commutation_discriminator_is_one_dpor_schedule() {
    const model::Program program{{
        {rlock("rw"), read("shared"), runlock("rw")},
        {rlock("rw"), read("shared"), runlock("rw")},
        {rlock("rw"), read("shared"), runlock("rw")},
    }};
    const model::ModelChecker checker(program);
    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    require(naive.schedules_explored == 1680,
            "three-reader naive leaf count changed from 9!/(3!^3)");
    require(dpor.schedules_explored == 1,
            "three-reader reader-mode commutation did not reduce to one DPOR schedule");
    require_clean(naive, "three-reader naive discriminator reported a bug");
    require_clean(dpor, "three-reader DPOR discriminator reported a bug");
}

} // namespace

int main() {
    writer_release_to_reader_acquire_probe();
    writer_release_to_writer_acquire_probe();
    every_reader_release_to_writer_acquire_probe();
    readers_do_not_synchronize_with_each_other_probe();
    parallel_readers_and_exclusive_writers_are_race_free();
    unlocked_reader_races_with_writer();
    non_holder_unlocks_are_modeled_errors();
    reentrancy_is_a_modeled_error_but_upgrade_is_a_deadlock();
    deadlock_reports_distinguish_writer_and_reader_blockers();
    buffered_rwlock_actions_wait_for_store_buffer_drain();
    mutex_and_rwlock_namespaces_cannot_mix();
    independence_clauses_match_the_proved_scope();
    terminal_reentrant_reader_does_not_prune_enabled_siblings();
    three_reader_commutation_discriminator_is_one_dpor_schedule();
    return 0;
}
