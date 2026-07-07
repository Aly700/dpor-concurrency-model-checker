#include "model/checker.hpp"
#include "program_parser.hpp"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <string>
#include <utility>

namespace cli {

model::Program parse_program_text(const std::string& text);
std::string render_program(const model::Program& program);

} // namespace cli

namespace {

model::Action write(std::string address) {
    model::Action action;
    action.kind = model::ActionKind::Write;
    action.address = std::move(address);
    return action;
}

model::Action yield() {
    model::Action action;
    action.kind = model::ActionKind::Yield;
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

bool has_blocked_joiner(const model::DeadlockReport& report,
                        model::ThreadId thread,
                        model::ThreadId target) {
    return std::any_of(report.blocked_threads.begin(), report.blocked_threads.end(), [&](const auto& blocked) {
        return blocked.thread == thread &&
               blocked.kind == model::BlockedOnKind::Thread &&
               blocked.target.has_value() &&
               *blocked.target == target;
    });
}

void assert_clean(const model::CheckResult& result) {
    assert(!result.first_race.has_value());
    assert(!result.first_deadlock.has_value());
    assert(!result.first_error.has_value());
}

void assert_naive_dpor_agree(const model::Program& program) {
    const model::ModelChecker checker(program);
    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    assert(naive.first_race.has_value() == dpor.first_race.has_value());
    assert(naive.first_deadlock.has_value() == dpor.first_deadlock.has_value());
    assert(naive.first_error.has_value() == dpor.first_error.has_value());
    assert(dpor.schedules_explored <= naive.schedules_explored);

    if (dpor.first_race.has_value()) {
        const auto replay = checker.replay(dpor.first_race->schedule);
        assert(replay.first_race.has_value());
        assert(*replay.first_race == *dpor.first_race);
    }
    if (dpor.first_deadlock.has_value()) {
        const auto replay = checker.replay(dpor.first_deadlock->schedule);
        assert(replay.first_deadlock.has_value());
        assert(*replay.first_deadlock == *dpor.first_deadlock);
    }
    if (dpor.first_error.has_value()) {
        const auto replay = checker.replay(dpor.first_error->schedule);
        assert(replay.first_error.has_value());
        assert(*replay.first_error == *dpor.first_error);
    }
}

void spawn_happens_before_suppresses_pre_spawn_write_race() {
    const model::Program program{{
        {write("x"), spawn(1)},
        {write("x")},
    }};

    const model::ModelChecker checker(program);
    const auto replay = checker.replay({{0, 0}, {0, 1}, {1, 0}});
    assert(replay.schedules_explored == 1);
    assert_clean(replay);

    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    assert_clean(naive);
    assert_clean(dpor);
    assert_naive_dpor_agree(program);
}

void post_spawn_write_races_with_spawned_thread_write() {
    const model::Program program{{
        {spawn(1), write("x")},
        {write("x")},
    }};

    const model::ModelChecker checker(program);
    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    assert(naive.first_race.has_value());
    assert(dpor.first_race.has_value());
    assert(naive.first_race->address == "x");
    assert(dpor.first_race->address == "x");
    assert_naive_dpor_agree(program);
}

void empty_body_spawn_target_does_not_satisfy_join_before_start() {
    const model::Program program{{
        {join(1)},
        {},
        {spawn(1), spawn(3)},
        {spawn(2)},
    }};

    const model::ModelChecker checker(program);
    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    assert(naive.first_deadlock.has_value());
    assert(dpor.first_deadlock.has_value());
    assert(has_blocked_joiner(*naive.first_deadlock, 0, 1));
    assert(has_blocked_joiner(*dpor.first_deadlock, 0, 1));
    assert(naive.first_deadlock->schedule.empty());
    assert(dpor.first_deadlock->schedule.empty());
    assert_naive_dpor_agree(program);
}

void unspawned_threads_do_not_deadlock_clean_termination() {
    const model::Program program{{
        {yield()},
        {},
        {spawn(1), spawn(3)},
        {spawn(2)},
    }};

    const model::ModelChecker checker(program);
    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    assert_clean(naive);
    assert_clean(dpor);
    assert_naive_dpor_agree(program);
}

void self_spawn_is_modeled_error() {
    const model::Program program{{
        {spawn(0)},
    }};

    const model::ModelChecker checker(program);
    const auto result = checker.explore_naive();
    assert(result.schedules_explored == 1);
    assert(result.first_error.has_value());
    assert(result.first_error->endpoint.thread == 0);
    assert(result.first_error->endpoint.action_index == 0);
    assert(result.first_error->message.find("spawn") != std::string::npos);
    assert_naive_dpor_agree(program);
}

void out_of_range_spawn_is_modeled_error() {
    const model::Program program{{
        {spawn(7)},
    }};

    const model::ModelChecker checker(program);
    const auto result = checker.explore_naive();
    assert(result.schedules_explored == 1);
    assert(result.first_error.has_value());
    assert(result.first_error->endpoint.thread == 0);
    assert(result.first_error->endpoint.action_index == 0);
    assert(result.first_error->message.find("out-of-range") != std::string::npos);
    assert_naive_dpor_agree(program);
}

void double_spawn_reports_already_started() {
    const model::Program program{{
        {spawn(1), spawn(1)},
        {yield()},
    }};

    const model::ModelChecker checker(program);
    const auto result = checker.explore_naive();
    assert(result.first_error.has_value());
    assert(result.first_error->endpoint.thread == 0);
    assert(result.first_error->endpoint.action_index == 1);
    assert(result.first_error->message.find("already started") != std::string::npos);
    assert_naive_dpor_agree(program);
}

void spawn_join_pipeline_is_clean_in_all_schedules() {
    const model::Program program{{
        {write("x"), spawn(1), join(1), write("y")},
        {write("x")},
    }};

    const model::ModelChecker checker(program);
    const auto naive = checker.explore_naive();
    const auto dpor = checker.explore_dpor();
    assert_clean(naive);
    assert_clean(dpor);
    assert_naive_dpor_agree(program);
}

void minimization_rejects_candidates_that_delete_required_spawn() {
    const model::Program program{{
        {spawn(1), yield()},
        {yield(), write("x")},
        {write("x")},
    }};
    const model::Schedule padded = {
        {0, 0}, {0, 1}, {1, 0}, {2, 0}, {1, 1},
    };

    const model::ModelChecker checker(program);
    const auto raw = checker.replay(padded);
    assert(raw.first_race.has_value());

    const auto minimized = checker.minimize_schedule(padded);
    assert(std::find(minimized.begin(), minimized.end(), model::ScheduleStep{0, 0}) != minimized.end());
    assert(std::find(minimized.begin(), minimized.end(), model::ScheduleStep{1, 1}) != minimized.end());

    const auto replay = checker.replay(minimized);
    assert(replay.first_race.has_value());
    assert(replay.first_race->address == raw.first_race->address);
}

void spawn_keyword_round_trips() {
    const std::string text =
        "thread:\n"
        "  write x\n"
        "  spawn 1\n"
        "  join 1\n"
        "thread:\n"
        "  write x\n";

    const model::Program parsed = cli::parse_program_text(text);
    assert(parsed.threads.size() == 2);
    assert(parsed.threads[0][1] == spawn(1));
    assert(cli::parse_program_text(cli::render_program(parsed)).threads == parsed.threads);
}

void parser_rejects_out_of_range_spawn_target() {
    bool threw = false;
    try {
        (void)cli::parse_program_text(
            "thread:\n"
            "  spawn 1\n");
    } catch (const cli::ParseError& error) {
        threw = true;
        assert(error.line() == 2);
        assert(std::string(error.what()).find("spawn target") != std::string::npos);
    }
    assert(threw);
}

void parser_rejects_self_spawn_target() {
    bool threw = false;
    try {
        (void)cli::parse_program_text(
            "thread:\n"
            "  spawn 0\n");
    } catch (const cli::ParseError& error) {
        threw = true;
        assert(error.line() == 2);
        assert(std::string(error.what()).find("self") != std::string::npos);
    }
    assert(threw);
}

void parser_rejects_duplicate_spawn_target() {
    bool threw = false;
    try {
        (void)cli::parse_program_text(
            "thread:\n"
            "  spawn 1\n"
            "  spawn 1\n"
            "thread:\n"
            "  yield\n");
    } catch (const cli::ParseError& error) {
        threw = true;
        assert(error.line() == 3);
        assert(std::string(error.what()).find("more than one spawn") != std::string::npos);
    }
    assert(threw);
}

} // namespace

int main() {
    spawn_happens_before_suppresses_pre_spawn_write_race();
    post_spawn_write_races_with_spawned_thread_write();
    empty_body_spawn_target_does_not_satisfy_join_before_start();
    unspawned_threads_do_not_deadlock_clean_termination();
    self_spawn_is_modeled_error();
    out_of_range_spawn_is_modeled_error();
    double_spawn_reports_already_started();
    spawn_join_pipeline_is_clean_in_all_schedules();
    minimization_rejects_candidates_that_delete_required_spawn();
    spawn_keyword_round_trips();
    parser_rejects_out_of_range_spawn_target();
    parser_rejects_self_spawn_target();
    parser_rejects_duplicate_spawn_target();
    return 0;
}
