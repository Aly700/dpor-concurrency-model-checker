#include "model/checker.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

model::Action rwlock_action(model::ActionKind kind, std::string name) {
    model::Action action;
    action.kind = kind;
    action.rwlock = std::move(name);
    return action;
}

model::Action rlock(std::string name) {
    return rwlock_action(model::ActionKind::RLock, std::move(name));
}

model::Action runlock(std::string name) {
    return rwlock_action(model::ActionKind::RUnlock, std::move(name));
}

model::Action wlock(std::string name) {
    return rwlock_action(model::ActionKind::WLock, std::move(name));
}

model::Action wunlock(std::string name) {
    return rwlock_action(model::ActionKind::WUnlock, std::move(name));
}

model::Action upgrade(std::string name) {
    return rwlock_action(model::ActionKind::Upgrade, std::move(name));
}

model::Action downgrade(std::string name) {
    return rwlock_action(model::ActionKind::Downgrade, std::move(name));
}

model::Action barrier_wait(std::string name, std::uint32_t parties) {
    model::Action action;
    action.kind = model::ActionKind::BarrierWait;
    action.barrier = std::move(name);
    action.parties = parties;
    return action;
}

model::ValueOperand immediate(model::Value value) {
    model::ValueOperand operand;
    operand.kind = model::ValueOperandKind::Immediate;
    operand.immediate = value;
    return operand;
}

model::Action write(std::string address, model::Value value) {
    model::Action action;
    action.kind = model::ActionKind::Write;
    action.address = std::move(address);
    action.value = immediate(value);
    return action;
}

model::Action read(std::string address, model::RegisterId destination) {
    model::Action action;
    action.kind = model::ActionKind::Read;
    action.address = std::move(address);
    action.destination = destination;
    return action;
}

model::Action assert_nonzero(model::RegisterId source) {
    model::Action action;
    action.kind = model::ActionKind::Assert;
    action.source_register = source;
    return action;
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
    require(!result.exploration_capped, message);
}

void require_replays_reports(const model::ModelChecker& checker,
                             const model::CheckResult& result) {
    if (result.first_deadlock.has_value()) {
        const model::CheckResult replay =
            checker.replay(result.first_deadlock->schedule);
        require(replay.first_deadlock.has_value() &&
                    *replay.first_deadlock == *result.first_deadlock,
                "rwlock conversion deadlock did not replay identically");
    }
    if (result.first_error.has_value()) {
        const model::CheckResult replay =
            checker.replay(result.first_error->schedule);
        require(replay.first_error.has_value() &&
                    *replay.first_error == *result.first_error,
                "rwlock conversion error did not replay identically");
    }
    if (result.first_assertion.has_value()) {
        const model::CheckResult replay =
            checker.replay(result.first_assertion->schedule);
        require(replay.first_assertion.has_value() &&
                    *replay.first_assertion == *result.first_assertion,
                "rwlock conversion assertion did not replay identically");
    }
}

void valid_conversions_change_ownership_atomically() {
    for (const model::Program& program : {
             model::Program{{{rlock("rw"), upgrade("rw"), wunlock("rw")}}},
             model::Program{{{wlock("rw"), downgrade("rw"), runlock("rw")}}},
         }) {
        const model::ModelChecker checker(program);
        require_clean(checker.explore_naive(),
                      "valid rwlock conversion was not clean under naive exploration");
        require_clean(checker.explore_dpor(),
                      "valid rwlock conversion was not clean under DPOR");
    }

    for (const model::Program& wrong_unlock : {
             model::Program{{{rlock("rw"), upgrade("rw"), runlock("rw")}}},
             model::Program{{{wlock("rw"), downgrade("rw"), wunlock("rw")}}},
         }) {
        const model::ModelChecker checker(wrong_unlock);
        const model::CheckResult result = checker.explore_naive();
        require(result.first_error.has_value(),
                "conversion left the caller holding the old ownership mode");
        require(result.first_error->endpoint.action_index == 2,
                "post-conversion wrong-mode error used the wrong endpoint");
        require_replays_reports(checker, result);
    }
}

void conversion_misuse_matches_the_mismatched_unlock_convention() {
    struct MisuseCase {
        model::Program program;
        std::uint32_t endpoint;
        std::string message;
    };
    const MisuseCase cases[] = {
        {model::Program{{{upgrade("rw")}}},
         0,
         "thread 0 attempted to upgrade rwlock 'rw' but it does not hold that mode"},
        {model::Program{{{wlock("rw"), upgrade("rw")}}},
         1,
         "thread 0 attempted to upgrade rwlock 'rw' but it does not hold that mode"},
        {model::Program{{{downgrade("rw")}}},
         0,
         "thread 0 attempted to downgrade rwlock 'rw' but it does not hold that mode"},
        {model::Program{{{rlock("rw"), downgrade("rw")}}},
         1,
         "thread 0 attempted to downgrade rwlock 'rw' but it does not hold that mode"},
    };

    for (const MisuseCase& test_case : cases) {
        const model::ModelChecker checker(test_case.program);
        for (const model::CheckResult& result :
             {checker.explore_naive(), checker.explore_dpor()}) {
            require(result.first_error.has_value(),
                    "rwlock conversion misuse was not a modeled error");
            require(result.first_error->endpoint.action_index ==
                        test_case.endpoint,
                    "rwlock conversion misuse used the wrong endpoint");
            require(result.first_error->message == test_case.message,
                    "rwlock conversion misuse did not match the house message");
            require_replays_reports(checker, result);
        }
    }
}

void transient_reader_schedule_classes_are_pinned() {
    const model::Program program{{
        {rlock("rw"), upgrade("rw"), wunlock("rw")},
        {rlock("rw"), runlock("rw")},
    }};
    const model::ModelChecker checker(program);
    const model::CheckResult naive = checker.explore_naive();
    const model::CheckResult dpor = checker.explore_dpor();
    require_clean(naive, "transient-reader naive fixture was not clean");
    require_clean(dpor, "transient-reader DPOR fixture was not clean");
    require(naive.schedules_explored == 4,
            "transient-reader naive class count changed");
    require(dpor.schedules_explored == 4,
            "Upgrade-bearing DPOR did not retain all transient-reader classes");
}

bool is_upgrade_blocker(const model::BlockedThread& blocked,
                        model::ThreadId thread) {
    return blocked.thread == thread &&
           blocked.kind == model::BlockedOnKind::RwLockUpgrade &&
           blocked.rwlock == "rw" &&
           !blocked.owner.has_value() &&
           !blocked.self_wait;
}

void synchronized_double_upgrade_has_distinct_blockers() {
    const model::Program program{{
        {rlock("rw"), barrier_wait("ready", 2), upgrade("rw")},
        {rlock("rw"), barrier_wait("ready", 2), upgrade("rw")},
    }};
    const model::ModelChecker checker(program);
    const model::CheckResult naive = checker.explore_naive();
    const model::CheckResult dpor = checker.explore_dpor();
    for (const model::CheckResult* result : {&naive, &dpor}) {
        require(result->first_deadlock.has_value(),
                "double Upgrade did not surface as deadlock");
        require(!result->first_error.has_value(),
                "double Upgrade was mislabeled as misuse");
        const auto& blocked = result->first_deadlock->blocked_threads;
        require(blocked.size() == 2,
                "double Upgrade deadlock omitted a blocked reader");
        require(std::any_of(blocked.begin(), blocked.end(),
                            [](const model::BlockedThread& entry) {
                                return is_upgrade_blocker(entry, 0);
                            }) &&
                    std::any_of(blocked.begin(), blocked.end(),
                                [](const model::BlockedThread& entry) {
                                    return is_upgrade_blocker(entry, 1);
                                }),
                "double Upgrade used the wrong blocker identity");
        require_replays_reports(checker, *result);
    }
    require(naive.schedules_explored == 6,
            "double Upgrade naive arrival-class count changed");
    require(dpor.schedules_explored == 4,
            "Upgrade-aware DPOR retained the wrong deadlock classes");
}

void upgrade_bearing_program_restricts_rlock_commutation() {
    const model::Program upgrade_free{{
        {rlock("rw"), runlock("rw")},
        {rlock("rw"), runlock("rw")},
    }};
    const model::ModelChecker legacy_checker(upgrade_free);
    require(legacy_checker.dpor_transitions_independent(
                0, rlock("rw"), 1, rlock("rw")),
            "Upgrade-free program lost RLock/RLock commutation");

    const model::Program upgrade_present{{
        {rlock("rw"), upgrade("rw"), wunlock("rw")},
        {rlock("rw"), upgrade("rw"), wunlock("rw")},
    }};
    const model::ModelChecker guarded_checker(upgrade_present);
    require(!model::independent(rlock("rw"), rlock("rw")),
            "public action-only relation exposed Upgrade-sensitive commutation");
    require(!guarded_checker.dpor_transitions_independent(
                0, rlock("rw"), 1, rlock("rw")),
            "Upgrade-bearing program bypassed the static restriction");

    const model::Program per_name_scope{{
        {upgrade("other")},
        {rlock("rw")},
    }};
    const model::ModelChecker per_name_checker(per_name_scope);
    require(per_name_checker.dpor_transitions_independent(
                0, rlock("rw"), 1, rlock("rw")),
            "Upgrade on a distinct rwlock leaked into the RLock/RLock guard");
}

void asymmetric_upgrader_first_class_is_not_pruned() {
    const model::Program program{{
        {rlock("rw"), upgrade("rw"), write("flag", 1), wunlock("rw")},
        {rlock("rw"), upgrade("rw"), read("flag", 0), wunlock("rw"),
         assert_nonzero(0)},
    }};
    const model::ModelChecker checker(program);
    const model::CheckResult naive = checker.explore_naive();
    const model::CheckResult dpor = checker.explore_dpor();
    require(naive.first_assertion.has_value() &&
                dpor.first_assertion.has_value(),
            "DPOR pruned the thread-1-upgrades-first assertion class");
    require(naive.first_deadlock.has_value() &&
                dpor.first_deadlock.has_value(),
            "DPOR pruned the both-readers-upgrade deadlock class");
    require(!naive.first_error.has_value() && !dpor.first_error.has_value(),
            "asymmetric upgrade discriminator produced a modeled error");
    require(!naive.exploration_capped && !dpor.exploration_capped &&
                naive.bound_exceeded_executions == 0 &&
                dpor.bound_exceeded_executions == 0,
            "asymmetric upgrade discriminator was not exhaustive");
    require_replays_reports(checker, naive);
    require_replays_reports(checker, dpor);
}

void buffered_conversions_wait_for_store_buffer_drain() {
    for (const model::MemoryModel memory_model : {model::MemoryModel::TSO,
                                                  model::MemoryModel::PSO}) {
        for (const bool upgrades : {true, false}) {
            const model::Program program{{{
                upgrades ? rlock("rw") : wlock("rw"),
                write("x", 1),
                upgrades ? upgrade("rw") : downgrade("rw"),
                upgrades ? wunlock("rw") : runlock("rw"),
            }}};
            const model::ModelChecker checker(program, 20, memory_model);

            bool rejected_before_drain = false;
            try {
                (void)checker.replay({
                    {0, 0},
                    {0, 1},
                    {0, 2},
                });
            } catch (const std::invalid_argument&) {
                rejected_before_drain = true;
            }
            require(
                rejected_before_drain,
                upgrades
                    ? "Upgrade executed before its store buffer drained"
                    : "Downgrade executed before its store buffer drained");

            const model::CheckResult replay = checker.replay({
                {0, 0},
                {0, 1},
                flush_step(0, memory_model),
                {0, 2},
                {0, 3},
            });
            require_clean(
                replay,
                upgrades
                    ? "Upgrade did not execute cleanly after store-buffer drain"
                    : "Downgrade did not execute cleanly after store-buffer drain");
        }
    }
}

} // namespace

int main() {
    valid_conversions_change_ownership_atomically();
    conversion_misuse_matches_the_mismatched_unlock_convention();
    transient_reader_schedule_classes_are_pinned();
    synchronized_double_upgrade_has_distinct_blockers();
    upgrade_bearing_program_restricts_rlock_commutation();
    asymmetric_upgrader_first_class_is_not_pruned();
    buffered_conversions_wait_for_store_buffer_drain();
    return 0;
}
