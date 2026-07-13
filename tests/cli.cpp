#include <sys/wait.h>

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

struct CommandResult {
    int exit_code{0};
    std::string stdout_text;
    std::string stderr_text;
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("could not read test file");
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("could not write test file");
    }
    output << text;
}

std::string shell_quote(const std::filesystem::path& path) {
    std::string input = path.string();
    std::string quoted = "'";
    for (const char character : input) {
        if (character == '\'') {
            quoted += "'\\''";
        } else {
            quoted += character;
        }
    }
    quoted += "'";
    return quoted;
}

int decode_exit_status(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return 128;
}

CommandResult run_command(const std::filesystem::path& binary,
                          const std::vector<std::string>& args,
                          const std::filesystem::path& stdout_path,
                          const std::filesystem::path& stderr_path) {
    std::string command = shell_quote(binary);
    for (const std::string& arg : args) {
        command += " ";
        command += shell_quote(arg);
    }
    command += " > ";
    command += shell_quote(stdout_path);
    command += " 2> ";
    command += shell_quote(stderr_path);

    const int status = std::system(command.c_str());
    return CommandResult{
        decode_exit_status(status),
        read_file(stdout_path),
        read_file(stderr_path),
    };
}

std::filesystem::path example(const std::filesystem::path& source_dir, const std::string& name) {
    return source_dir / "examples" / name;
}

[[maybe_unused]] std::filesystem::path golden(const std::filesystem::path& source_dir,
                                              const std::string& name) {
    return source_dir / "tests" / "golden" / name;
}

std::string first_line(const std::string& text) {
    const std::size_t newline = text.find('\n');
    if (newline == std::string::npos) {
        return text;
    }
    return text.substr(0, newline);
}

[[maybe_unused]] std::string details_for_round_trip(const std::string& report) {
    std::istringstream input(report);
    std::ostringstream details;
    std::string line;
    bool copying = true;
    while (copying && std::getline(input, line)) {
        if (line.rfind("schedules_explored:", 0) == 0 ||
            line.rfind("cycles_detected:", 0) == 0 ||
            line.rfind("bound_exceeded_executions:", 0) == 0 ||
            line.rfind("exploration_capped:", 0) == 0 ||
            line.rfind("also_found:", 0) == 0 ||
            line == "trace:" ||
            line == "schedule:") {
            if (line == "trace:" || line == "schedule:") {
                copying = false;
            }
            continue;
        }
        details << line << '\n';
    }
    return details.str();
}

std::string schedule_block(const std::string& report) {
    const std::string marker = "schedule:\n";
    const std::size_t offset = report.find(marker);
    if (offset == std::string::npos) {
        throw std::runtime_error("report did not contain schedule block");
    }
    return report.substr(offset + marker.size());
}

void assert_clean_program_exits_zero(const std::filesystem::path& binary,
                                     const std::filesystem::path& source_dir,
                                     const std::filesystem::path& work_dir) {
    const auto result = run_command(
        binary,
        {"check", example(source_dir, "clean_locked_counter.dpor").string(),
         "--explorer", "dpor", "--max-schedules", "1000"},
        work_dir / "cli_clean.out",
        work_dir / "cli_clean.err");
    assert(result.exit_code == 0);
    assert(result.stderr_text.empty());
    assert(first_line(result.stdout_text) == "verdict: clean");
}

void assert_race_golden_and_round_trip(const std::filesystem::path& binary,
                                       const std::filesystem::path& source_dir,
                                       const std::filesystem::path& work_dir) {
    const auto check = run_command(
        binary,
        {"check", example(source_dir, "data_race.dpor").string(), "--explorer", "naive"},
        work_dir / "cli_race.out",
        work_dir / "cli_race.err");
    assert(check.exit_code == 1);
    assert(check.stderr_text.empty());
    assert(check.stdout_text == read_file(golden(source_dir, "data_race_naive.txt")));

    const auto schedule_path = work_dir / "cli_race.schedule";
    write_file(schedule_path, schedule_block(check.stdout_text));
    const auto replay = run_command(
        binary,
        {"replay", example(source_dir, "data_race.dpor").string(), "--schedule", schedule_path.string()},
        work_dir / "cli_race_replay.out",
        work_dir / "cli_race_replay.err");
    assert(replay.exit_code == 1);
    assert(replay.stderr_text.empty());
    assert(details_for_round_trip(replay.stdout_text) == details_for_round_trip(check.stdout_text));
}

void assert_deadlock_golden_and_round_trip(const std::filesystem::path& binary,
                                           const std::filesystem::path& source_dir,
                                           const std::filesystem::path& work_dir) {
    const auto check = run_command(
        binary,
        {"check", example(source_dir, "ab_ba_deadlock.dpor").string(), "--explorer", "naive"},
        work_dir / "cli_deadlock.out",
        work_dir / "cli_deadlock.err");
    assert(check.exit_code == 1);
    assert(check.stderr_text.empty());
    assert(check.stdout_text == read_file(golden(source_dir, "ab_ba_deadlock_naive.txt")));

    const auto schedule_path = work_dir / "cli_deadlock.schedule";
    write_file(schedule_path, schedule_block(check.stdout_text));
    const auto replay = run_command(
        binary,
        {"replay", example(source_dir, "ab_ba_deadlock.dpor").string(), "--schedule", schedule_path.string()},
        work_dir / "cli_deadlock_replay.out",
        work_dir / "cli_deadlock_replay.err");
    assert(replay.exit_code == 1);
    assert(replay.stderr_text.empty());
    assert(details_for_round_trip(replay.stdout_text) == details_for_round_trip(check.stdout_text));
}

void assert_lost_wakeup_round_trips_with_wait_phases(const std::filesystem::path& binary,
                                                     const std::filesystem::path& source_dir,
                                                     const std::filesystem::path& work_dir) {
    const auto check = run_command(
        binary,
        {"check", example(source_dir, "lost_wakeup.dpor").string(), "--explorer", "naive"},
        work_dir / "cli_lost_wakeup.out",
        work_dir / "cli_lost_wakeup.err");
    assert(check.exit_code == 1);
    assert(check.stderr_text.empty());
    assert(first_line(check.stdout_text) == "verdict: deadlock");
    assert(check.stdout_text.find("wait cv m (sleep)") != std::string::npos);
    assert(check.stdout_text.find("wait cv m (reacquire)") != std::string::npos);

    const auto schedule_path = work_dir / "cli_lost_wakeup.schedule";
    write_file(schedule_path, schedule_block(check.stdout_text));
    const auto replay = run_command(
        binary,
        {"replay", example(source_dir, "lost_wakeup.dpor").string(), "--schedule", schedule_path.string()},
        work_dir / "cli_lost_wakeup_replay.out",
        work_dir / "cli_lost_wakeup_replay.err");
    assert(replay.exit_code == 1);
    assert(replay.stderr_text.empty());
    assert(details_for_round_trip(replay.stdout_text) == details_for_round_trip(check.stdout_text));
}

void assert_modeled_error_reports_exit_one(const std::filesystem::path& binary,
                                           const std::filesystem::path& source_dir,
                                           const std::filesystem::path& work_dir) {
    const auto result = run_command(
        binary,
        {"check", example(source_dir, "unlock_error.dpor").string()},
        work_dir / "cli_error.out",
        work_dir / "cli_error.err");
    assert(result.exit_code == 1);
    assert(result.stderr_text.empty());
    assert(first_line(result.stdout_text) == "verdict: error");
    assert(result.stdout_text.find("attempted to unlock mutex 'm' but it is not owned") != std::string::npos);
}

void assert_parse_error_has_line_number(const std::filesystem::path& binary,
                                        const std::filesystem::path& source_dir,
                                        const std::filesystem::path& work_dir) {
    const auto program_path = work_dir / "cli_bad_parse.dpor";
    write_file(program_path, "thread:\n  write x\n  wriet x\n");
    const auto result = run_command(
        binary,
        {"check", program_path.string()},
        work_dir / "cli_parse.out",
        work_dir / "cli_parse.err");
    assert(result.exit_code == 2);
    assert(result.stdout_text.empty());
    assert(result.stderr_text.find("line 3") != std::string::npos);
    assert(result.stderr_text.find("unknown keyword") != std::string::npos);
    (void)source_dir;
}

void assert_invalid_schedule_exits_two(const std::filesystem::path& binary,
                                       const std::filesystem::path& source_dir,
                                       const std::filesystem::path& work_dir) {
    const auto schedule_path = work_dir / "cli_invalid.schedule";
    write_file(schedule_path, "0 0\n1 0\n");
    const auto result = run_command(
        binary,
        {"replay", example(source_dir, "clean_locked_counter.dpor").string(), "--schedule", schedule_path.string()},
        work_dir / "cli_invalid.out",
        work_dir / "cli_invalid.err");
    assert(result.exit_code == 2);
    assert(result.stdout_text.empty());
    assert(result.stderr_text.find("invalid replay schedule") != std::string::npos);
}

void assert_explorer_flags_work_and_agree(const std::filesystem::path& binary,
                                          const std::filesystem::path& source_dir,
                                          const std::filesystem::path& work_dir) {
    const auto naive = run_command(
        binary,
        {"check", example(source_dir, "message_passing_atomics.dpor").string(), "--explorer", "naive"},
        work_dir / "cli_naive.out",
        work_dir / "cli_naive.err");
    const auto dpor = run_command(
        binary,
        {"check", example(source_dir, "message_passing_atomics.dpor").string(), "--explorer", "dpor"},
        work_dir / "cli_dpor.out",
        work_dir / "cli_dpor.err");
    const auto defaulted = run_command(
        binary,
        {"check", example(source_dir, "message_passing_atomics.dpor").string()},
        work_dir / "cli_default.out",
        work_dir / "cli_default.err");
    assert(naive.exit_code == 1);
    assert(dpor.exit_code == 1);
    assert(defaulted.exit_code == 1);
    assert(naive.stderr_text.empty());
    assert(dpor.stderr_text.empty());
    assert(defaulted.stderr_text.empty());
    assert(first_line(naive.stdout_text) == first_line(dpor.stdout_text));
    assert(first_line(dpor.stdout_text) == first_line(defaulted.stdout_text));
}

void assert_assertion_reports_round_trip(const std::filesystem::path& binary,
                                         const std::filesystem::path& work_dir) {
    const auto program_path = work_dir / "cli_assertion.dpor";
    write_file(program_path, "thread:\n  assert r0\n");

    const auto check = run_command(
        binary,
        {"check", program_path.string(), "--explorer", "dpor"},
        work_dir / "cli_assertion.out",
        work_dir / "cli_assertion.err");
    assert(check.exit_code == 1);
    assert(check.stderr_text.empty());
    assert(first_line(check.stdout_text) == "verdict: assertion");
    assert(check.stdout_text.find("assertion:") != std::string::npos);
    assert(check.stdout_text.find("register: r0") != std::string::npos);
    assert(check.stdout_text.find("value: 0") != std::string::npos);

    const auto schedule_path = work_dir / "cli_assertion.schedule";
    write_file(schedule_path, schedule_block(check.stdout_text));
    const auto replay = run_command(
        binary,
        {"replay", program_path.string(), "--schedule", schedule_path.string()},
        work_dir / "cli_assertion_replay.out",
        work_dir / "cli_assertion_replay.err");
    assert(replay.exit_code == 1);
    assert(replay.stderr_text.empty());
    assert(details_for_round_trip(replay.stdout_text) == details_for_round_trip(check.stdout_text));
}

void assert_tso_report_renders_flush_and_replays(const std::filesystem::path& binary,
                                                 const std::filesystem::path& work_dir) {
    const auto program_path = work_dir / "cli_tso_flush.dpor";
    write_file(program_path,
               "thread:\n"
               "  write x 1\n"
               "thread:\n"
               "  read x -> r0\n");

    const auto check = run_command(
        binary,
        {"check", program_path.string(), "--explorer", "dpor", "--memory-model", "tso"},
        work_dir / "cli_tso_flush.out",
        work_dir / "cli_tso_flush.err");
    assert(check.exit_code == 1);
    assert(check.stderr_text.empty());
    assert(first_line(check.stdout_text) == "verdict: race");
    assert(check.stdout_text.find("memory_model: tso\n") != std::string::npos);
    assert(check.stdout_text.find("flush x") != std::string::npos);
    assert(check.stdout_text.find("0 4294967295") != std::string::npos);

    const auto schedule_path = work_dir / "cli_tso_flush.schedule";
    write_file(schedule_path, schedule_block(check.stdout_text));
    const auto replay = run_command(
        binary,
        {"replay", program_path.string(), "--schedule", schedule_path.string(), "--memory-model", "tso"},
        work_dir / "cli_tso_flush_replay.out",
        work_dir / "cli_tso_flush_replay.err");
    assert(replay.exit_code == 1);
    assert(replay.stderr_text.empty());
    assert(details_for_round_trip(replay.stdout_text) == details_for_round_trip(check.stdout_text));
}

void assert_pso_report_renders_address_tagged_flush_and_replays(
    const std::filesystem::path& binary,
    const std::filesystem::path& work_dir) {
    const auto program_path = work_dir / "cli_pso_flush.dpor";
    write_file(program_path,
               "thread:\n"
               "  write x 1\n"
               "thread:\n"
               "  read x -> r0\n");

    const auto check = run_command(
        binary,
        {"check", program_path.string(), "--explorer", "dpor", "--memory-model", "pso"},
        work_dir / "cli_pso_flush.out",
        work_dir / "cli_pso_flush.err");
    assert(check.exit_code == 1);
    assert(check.stderr_text.empty());
    assert(first_line(check.stdout_text) == "verdict: race");
    assert(check.stdout_text.find("memory_model: pso\n") != std::string::npos);
    assert(check.stdout_text.find("flush x") != std::string::npos);
    assert(check.stdout_text.find("0 4294967295 0") != std::string::npos);

    const auto schedule_path = work_dir / "cli_pso_flush.schedule";
    write_file(schedule_path, schedule_block(check.stdout_text));
    const auto replay = run_command(
        binary,
        {"replay", program_path.string(), "--schedule", schedule_path.string(), "--memory-model", "pso"},
        work_dir / "cli_pso_flush_replay.out",
        work_dir / "cli_pso_flush_replay.err");
    assert(replay.exit_code == 1);
    assert(replay.stderr_text.empty());
    assert(details_for_round_trip(replay.stdout_text) == details_for_round_trip(check.stdout_text));
}

void assert_spin_cycle_reports_nontermination_and_round_trips(const std::filesystem::path& binary,
                                                              const std::filesystem::path& work_dir) {
    const auto program_path = work_dir / "cli_spin_bound.dpor";
    write_file(program_path,
               "thread:\n"
               "  set r1 1\n"
               "spin:\n"
               "  atomic_load f -> r0\n"
               "  bnz r0 done\n"
               "  bnz r1 spin\n"
               "done:\n"
               "  assert r1\n"
               "thread:\n"
               "  atomic_store f 1\n");

    const auto check = run_command(
        binary,
        {"check", program_path.string(), "--explorer", "dpor", "--step-bound", "5"},
        work_dir / "cli_spin_bound.out",
        work_dir / "cli_spin_bound.err");
    // Formerly clean up to bound: the unfair schedule that postpones thread 1
    // now closes an exact cycle. Residual short-bound outcomes may coexist,
    // but nontermination has the stronger verdict priority.
    assert(check.exit_code == 1);
    assert(check.stderr_text.empty());
    assert(first_line(check.stdout_text) == "verdict: nontermination");
    assert(check.stdout_text.find("cycles_detected: ") != std::string::npos);
    assert(check.stdout_text.find("nontermination:\n"
                                  "  fairness: unfair-schedule witness\n"
                                  "  stem:\n") != std::string::npos);
    assert(check.stdout_text.find("  cycle:\n") != std::string::npos);
    assert(check.stdout_text.find("bound_exceeded_executions: ") != std::string::npos);

    const auto schedule_path = work_dir / "cli_spin_cycle.schedule";
    write_file(schedule_path, schedule_block(check.stdout_text));
    const auto replay = run_command(
        binary,
        {"replay", program_path.string(), "--schedule", schedule_path.string()},
        work_dir / "cli_spin_cycle_replay.out",
        work_dir / "cli_spin_cycle_replay.err");
    assert(replay.exit_code == 1);
    assert(replay.stderr_text.empty());
    assert(details_for_round_trip(replay.stdout_text) == details_for_round_trip(check.stdout_text));
}

void assert_growing_loop_reports_clean_up_to_bound(const std::filesystem::path& binary,
                                                   const std::filesystem::path& work_dir) {
    const auto program_path = work_dir / "cli_growing_bound.dpor";
    write_file(program_path,
               "thread:\n"
               "  set r1 1\n"
               "grow:\n"
               "  atomic_rmw counter 1 -> r0\n"
               "  bnz r1 grow\n");

    const auto check = run_command(
        binary,
        {"check", program_path.string(), "--explorer", "dpor", "--step-bound", "5"},
        work_dir / "cli_growing_bound.out",
        work_dir / "cli_growing_bound.err");
    assert(check.exit_code == 0);
    assert(check.stderr_text.empty());
    assert(first_line(check.stdout_text) == "verdict: clean up to bound");
    assert(check.stdout_text.find("cycles_detected:") == std::string::npos);
    assert(check.stdout_text.find("bound_exceeded_executions: 1\n") != std::string::npos);
}

void require_cli(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void assert_check_replay_byte_identity(
    const std::filesystem::path& binary,
    const std::filesystem::path& work_dir,
    const std::string& stem,
    const std::string& program_text,
    const std::string& verdict,
    std::initializer_list<std::string> required_text) {
    const auto program_path = work_dir / (stem + ".dpor");
    write_file(program_path, program_text);

    const auto check = run_command(
        binary,
        {"check", program_path.string(), "--explorer", "dpor"},
        work_dir / (stem + ".check.out"),
        work_dir / (stem + ".check.err"));
    require_cli(check.exit_code == 1, stem + " check should report a witness");
    require_cli(check.stderr_text.empty(), stem + " check wrote stderr: " + check.stderr_text);
    require_cli(first_line(check.stdout_text) == "verdict: " + verdict,
                stem + " check verdict mismatch");
    for (const std::string& required : required_text) {
        require_cli(check.stdout_text.find(required) != std::string::npos,
                    stem + " report omitted '" + required + "'");
    }

    const auto schedule_path = work_dir / (stem + ".schedule");
    write_file(schedule_path, schedule_block(check.stdout_text));
    const auto replay = run_command(
        binary,
        {"replay", program_path.string(), "--schedule", schedule_path.string()},
        work_dir / (stem + ".replay.out"),
        work_dir / (stem + ".replay.err"));
    require_cli(replay.exit_code == 1, stem + " replay should reproduce a witness");
    require_cli(replay.stderr_text.empty(), stem + " replay wrote stderr: " + replay.stderr_text);
    require_cli(replay.stdout_text == check.stdout_text,
                stem + " check and replay reports were not byte-identical");
}

void assert_rwlock_syntax_and_witnesses_round_trip_byte_identically(
    const std::filesystem::path& binary,
    const std::filesystem::path& work_dir) {
    // This assertion witness exercises both parsing and report rendering for
    // all four strict spellings. A one-thread trace also makes the entire
    // check and replay reports byte-identical, including exploration counts.
    assert_check_replay_byte_identity(
        binary,
        work_dir,
        "cli_rwlock_spellings",
        "thread:\n"
        "  rlock rw\n"
        "  runlock rw\n"
        "  wlock rw\n"
        "  wunlock rw\n"
        "  assert r0\n",
        "assertion",
        {"rlock rw", "runlock rw", "wlock rw", "wunlock rw"});

    assert_check_replay_byte_identity(
        binary,
        work_dir,
        "cli_runlock_error",
        "thread:\n"
        "  runlock rw\n",
        "error",
        {"rwlock 'rw'", "runlock rw"});
    assert_check_replay_byte_identity(
        binary,
        work_dir,
        "cli_wunlock_error",
        "thread:\n"
        "  wunlock rw\n",
        "error",
        {"rwlock 'rw'", "wunlock rw"});

    assert_check_replay_byte_identity(
        binary,
        work_dir,
        "cli_rwlock_waiting_writer",
        "thread:\n"
        "  wlock rw\n"
        "  spawn 1\n"
        "  join 1\n"
        "thread:\n"
        "  rlock rw\n",
        "deadlock",
        {"rwlock rw waiting_for_writer owned_by 0"});
    assert_check_replay_byte_identity(
        binary,
        work_dir,
        "cli_rwlock_waiting_readers",
        "thread:\n"
        "  rlock rw\n"
        "  spawn 1\n"
        "  join 1\n"
        "thread:\n"
        "  wlock rw\n",
        "deadlock",
        {"rwlock rw waiting_for_readers_to_drain"});
    assert_check_replay_byte_identity(
        binary,
        work_dir,
        "cli_rwlock_self_wait",
        "thread:\n"
        "  rlock rw\n"
        "  wlock rw\n",
        "deadlock",
        {"rwlock rw waiting_for_readers_to_drain self_wait"});
}

void assert_rwlock_parser_is_strict_and_namespaces_are_distinct(
    const std::filesystem::path& binary,
    const std::filesystem::path& work_dir) {
    const auto uppercase_path = work_dir / "cli_rwlock_uppercase.dpor";
    write_file(uppercase_path, "thread:\n  RLock rw\n");
    const auto uppercase = run_command(
        binary,
        {"check", uppercase_path.string()},
        work_dir / "cli_rwlock_uppercase.out",
        work_dir / "cli_rwlock_uppercase.err");
    require_cli(uppercase.exit_code == 2, "uppercase RLock should be rejected");
    require_cli(uppercase.stdout_text.empty(), "uppercase RLock wrote stdout");
    require_cli(uppercase.stderr_text.find("line 2") != std::string::npos,
                "uppercase RLock error omitted its line");
    require_cli(uppercase.stderr_text.find("unknown keyword 'RLock'") != std::string::npos,
                "uppercase RLock was not reported as unknown");

    const std::vector<std::pair<std::string, std::string>> collisions = {
        {
            "mutex_then_rwlock",
            "thread:\n"
            "  wait cv shared\n"
            "  rlock shared\n",
        },
        {
            "rwlock_then_mutex",
            "thread:\n"
            "  wlock shared\n"
            "  wait cv shared\n",
        },
    };
    for (const auto& [stem, program_text] : collisions) {
        const auto program_path = work_dir / ("cli_" + stem + ".dpor");
        write_file(program_path, program_text);
        const auto result = run_command(
            binary,
            {"check", program_path.string()},
            work_dir / ("cli_" + stem + ".out"),
            work_dir / ("cli_" + stem + ".err"));
        require_cli(result.exit_code == 2, stem + " namespace collision should be rejected");
        require_cli(result.stdout_text.empty(), stem + " namespace collision wrote stdout");
        require_cli(result.stderr_text.find("line 3") != std::string::npos,
                    stem + " namespace collision omitted the conflict line");
        require_cli(result.stderr_text.find("mutex") != std::string::npos,
                    stem + " namespace collision omitted mutex");
        require_cli(result.stderr_text.find("rwlock") != std::string::npos,
                    stem + " namespace collision omitted rwlock");
        require_cli(result.stderr_text.find("line 2") != std::string::npos,
                    stem + " namespace collision omitted the first-use line");
    }
}

void assert_semaphore_syntax_and_witnesses_round_trip_byte_identically(
    const std::filesystem::path& binary,
    const std::filesystem::path& work_dir) {
    // The assertion witness exercises parsing and canonical trace rendering
    // for both semaphore actions. With one thread, check and replay reports
    // are byte-identical, including the exploration count.
    assert_check_replay_byte_identity(
        binary,
        work_dir,
        "cli_semaphore_spellings",
        "thread:\n"
        "  sem_post permits\n"
        "  sem_wait permits\n"
        "  assert r0\n",
        "assertion",
        {"sem_post permits", "sem_wait permits"});

    assert_check_replay_byte_identity(
        binary,
        work_dir,
        "cli_semaphore_zero_permit",
        "thread:\n"
        "  sem_wait permits\n",
        "deadlock",
        {"semaphore permits waiting_for_post"});
}

void assert_semaphore_parser_is_strict_and_namespace_is_distinct(
    const std::filesystem::path& binary,
    const std::filesystem::path& work_dir) {
    const std::vector<std::pair<std::string, std::string>> uppercase_cases = {
        {"SemPost", "SemPost permits"},
        {"SemWait", "SemWait permits"},
    };
    for (const auto& [keyword, action] : uppercase_cases) {
        const auto program_path = work_dir / ("cli_" + keyword + ".dpor");
        write_file(program_path, "thread:\n  " + action + "\n");
        const auto result = run_command(
            binary,
            {"check", program_path.string()},
            work_dir / ("cli_" + keyword + ".out"),
            work_dir / ("cli_" + keyword + ".err"));
        require_cli(result.exit_code == 2, keyword + " should be rejected");
        require_cli(result.stdout_text.empty(), keyword + " wrote stdout");
        require_cli(result.stderr_text.find("line 2") != std::string::npos,
                    keyword + " error omitted its line");
        require_cli(result.stderr_text.find("unknown keyword '" + keyword + "'") !=
                        std::string::npos,
                    keyword + " was not reported as unknown");
    }

    const std::vector<std::pair<std::string, std::string>> arity_cases = {
        {"sem_post_missing", "sem_post"},
        {"sem_post_extra", "sem_post permits extra"},
        {"sem_wait_missing", "sem_wait"},
        {"sem_wait_extra", "sem_wait permits extra"},
    };
    for (const auto& [stem, action] : arity_cases) {
        const auto program_path = work_dir / ("cli_" + stem + ".dpor");
        write_file(program_path, "thread:\n  " + action + "\n");
        const auto result = run_command(
            binary,
            {"check", program_path.string()},
            work_dir / ("cli_" + stem + ".out"),
            work_dir / ("cli_" + stem + ".err"));
        require_cli(result.exit_code == 2, stem + " should be rejected");
        require_cli(result.stdout_text.empty(), stem + " wrote stdout");
        require_cli(result.stderr_text.find("line 2") != std::string::npos,
                    stem + " error omitted its line");
        const std::string keyword = stem.rfind("sem_post", 0) == 0
            ? "sem_post"
            : "sem_wait";
        require_cli(result.stderr_text.find("keyword '" + keyword +
                                            "' expects 1 operand") != std::string::npos,
                    stem + " did not report strict arity");
    }

    struct CollisionCase {
        std::string stem;
        std::string program_text;
        std::string other_namespace;
    };
    const std::vector<CollisionCase> collisions = {
        {
            "semaphore_then_mutex",
            "thread:\n"
            "  sem_post shared\n"
            "  lock shared\n",
            "mutex",
        },
        {
            "mutex_then_semaphore",
            "thread:\n"
            "  lock shared\n"
            "  sem_wait shared\n",
            "mutex",
        },
        {
            "semaphore_then_rwlock",
            "thread:\n"
            "  sem_wait shared\n"
            "  rlock shared\n",
            "rwlock",
        },
        {
            "rwlock_then_semaphore",
            "thread:\n"
            "  wlock shared\n"
            "  sem_post shared\n",
            "rwlock",
        },
    };
    for (const CollisionCase& collision : collisions) {
        const auto program_path = work_dir / ("cli_" + collision.stem + ".dpor");
        write_file(program_path, collision.program_text);
        const auto result = run_command(
            binary,
            {"check", program_path.string()},
            work_dir / ("cli_" + collision.stem + ".out"),
            work_dir / ("cli_" + collision.stem + ".err"));
        require_cli(result.exit_code == 2,
                    collision.stem + " namespace collision should be rejected");
        require_cli(result.stdout_text.empty(),
                    collision.stem + " namespace collision wrote stdout");
        require_cli(result.stderr_text.find("line 3") != std::string::npos,
                    collision.stem + " collision omitted the conflict line");
        require_cli(result.stderr_text.find("semaphore") != std::string::npos,
                    collision.stem + " collision omitted semaphore");
        require_cli(result.stderr_text.find(collision.other_namespace) != std::string::npos,
                    collision.stem + " collision omitted " + collision.other_namespace);
        require_cli(result.stderr_text.find("line 2") != std::string::npos,
                    collision.stem + " collision omitted the first-use line");
    }
}

} // namespace

int main(int argc, char** argv) {
    assert(argc == 3);
    (void)argc;
    const std::filesystem::path binary = argv[1];
    const std::filesystem::path source_dir = argv[2];
    const std::filesystem::path work_dir = std::filesystem::current_path();

    assert_clean_program_exits_zero(binary, source_dir, work_dir);
    assert_race_golden_and_round_trip(binary, source_dir, work_dir);
    assert_deadlock_golden_and_round_trip(binary, source_dir, work_dir);
    assert_lost_wakeup_round_trips_with_wait_phases(binary, source_dir, work_dir);
    assert_modeled_error_reports_exit_one(binary, source_dir, work_dir);
    assert_parse_error_has_line_number(binary, source_dir, work_dir);
    assert_invalid_schedule_exits_two(binary, source_dir, work_dir);
    assert_explorer_flags_work_and_agree(binary, source_dir, work_dir);
    assert_assertion_reports_round_trip(binary, work_dir);
    assert_tso_report_renders_flush_and_replays(binary, work_dir);
    assert_pso_report_renders_address_tagged_flush_and_replays(binary, work_dir);
    assert_spin_cycle_reports_nontermination_and_round_trips(binary, work_dir);
    assert_growing_loop_reports_clean_up_to_bound(binary, work_dir);
    assert_rwlock_syntax_and_witnesses_round_trip_byte_identically(binary, work_dir);
    assert_rwlock_parser_is_strict_and_namespaces_are_distinct(binary, work_dir);
    assert_semaphore_syntax_and_witnesses_round_trip_byte_identically(binary, work_dir);
    assert_semaphore_parser_is_strict_and_namespace_is_distinct(binary, work_dir);
    return 0;
}
