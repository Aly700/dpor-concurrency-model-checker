#include <sys/wait.h>

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
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

std::filesystem::path golden(const std::filesystem::path& source_dir, const std::string& name) {
    return source_dir / "tests" / "golden" / name;
}

std::string first_line(const std::string& text) {
    const std::size_t newline = text.find('\n');
    if (newline == std::string::npos) {
        return text;
    }
    return text.substr(0, newline);
}

std::string details_for_round_trip(const std::string& report) {
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
    assert(check.stdout_text.find("nontermination:\n  stem:\n") != std::string::npos);
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

} // namespace

int main(int argc, char** argv) {
    assert(argc == 3);
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
    return 0;
}
