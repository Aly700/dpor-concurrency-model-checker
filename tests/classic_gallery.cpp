#include "model/checker.hpp"
#include "program_parser.hpp"
#include "report.hpp"

#include <sys/wait.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

enum class ExpectedVerdict {
    Clean,
    CleanUpToBound,
    Race,
    Assertion,
    Deadlock,
    NonTermination,
};

struct GalleryCase {
    std::string file;
    ExpectedVerdict expected;
    std::size_t step_bound;
    std::size_t max_schedules;
    bool broken;
    model::MemoryModel memory_model{model::MemoryModel::SC};
    std::optional<bool> assertion_exists;
    bool dpor_only{false};
    std::optional<model::Fairness> expected_fairness;

    GalleryCase(std::string file_value,
                ExpectedVerdict expected_value,
                std::size_t step_bound_value,
                std::size_t max_schedules_value,
                bool broken_value,
                model::MemoryModel memory_model_value = model::MemoryModel::SC,
                std::optional<bool> assertion_exists_value = std::nullopt,
                bool dpor_only_value = false,
                std::optional<model::Fairness> expected_fairness_value = std::nullopt)
        : file(std::move(file_value)),
          expected(expected_value),
          step_bound(step_bound_value),
          max_schedules(max_schedules_value),
          broken(broken_value),
          memory_model(memory_model_value),
          assertion_exists(assertion_exists_value),
          dpor_only(dpor_only_value),
          expected_fairness(expected_fairness_value) {}
};

struct CommandResult {
    int exit_code{0};
    std::string stdout_text;
    std::string stderr_text;
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        fail("could not read file: " + path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path);
    if (!output) {
        fail("could not write file: " + path.string());
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

std::filesystem::path gallery_file(const std::filesystem::path& source_dir, const std::string& name) {
    return source_dir / "examples" / "classic" / name;
}

std::string expected_text(ExpectedVerdict verdict) {
    switch (verdict) {
    case ExpectedVerdict::Clean:
        return "clean";
    case ExpectedVerdict::CleanUpToBound:
        return "clean up to bound";
    case ExpectedVerdict::Race:
        return "race";
    case ExpectedVerdict::Assertion:
        return "assertion";
    case ExpectedVerdict::Deadlock:
        return "deadlock";
    case ExpectedVerdict::NonTermination:
        return "nontermination";
    }
    fail("unknown expected verdict");
}

const char* memory_model_argument(model::MemoryModel memory_model) {
    switch (memory_model) {
    case model::MemoryModel::SC:
        return "sc";
    case model::MemoryModel::TSO:
        return "tso";
    case model::MemoryModel::PSO:
        return "pso";
    }
    fail("unknown memory model");
}

ExpectedVerdict verdict_kind(const model::CheckResult& result) {
    const std::string verdict = cli::verdict_of(result);
    if (verdict == "clean") {
        return ExpectedVerdict::Clean;
    }
    if (verdict == "clean up to bound") {
        return ExpectedVerdict::CleanUpToBound;
    }
    if (verdict == "race") {
        return ExpectedVerdict::Race;
    }
    if (verdict == "assertion") {
        return ExpectedVerdict::Assertion;
    }
    if (verdict == "deadlock") {
        return ExpectedVerdict::Deadlock;
    }
    if (verdict == "nontermination") {
        return ExpectedVerdict::NonTermination;
    }
    fail("unexpected verdict text: " + verdict);
}

void require_expected_verdict(const GalleryCase& test_case,
                              const model::CheckResult& result,
                              const std::string& explorer) {
    const ExpectedVerdict actual = verdict_kind(result);
    require(actual == test_case.expected,
            test_case.file + " " + explorer + " expected " + expected_text(test_case.expected) +
                " but got " + cli::verdict_of(result));
    if (test_case.expected == ExpectedVerdict::Clean) {
        require(result.bound_exceeded_executions == 0,
                test_case.file + " " + explorer + " should not hit the step bound");
        require(result.cycles_detected == 0,
                test_case.file + " " + explorer + " should not detect a cycle");
    }
    if (test_case.expected == ExpectedVerdict::CleanUpToBound) {
        require(result.bound_exceeded_executions > 0,
                test_case.file + " " + explorer + " should hit the step bound");
    }
    if (test_case.expected == ExpectedVerdict::NonTermination) {
        require(result.first_nontermination.has_value() && result.cycles_detected > 0,
                test_case.file + " " + explorer + " should report a nontermination cycle");
    }
    if (test_case.expected_fairness.has_value()) {
        require(result.first_nontermination.has_value() &&
                    result.first_nontermination->fairness == *test_case.expected_fairness,
                test_case.file + " " + explorer + " fairness classification mismatch");
    }
    if (test_case.assertion_exists.has_value()) {
        require(result.first_assertion.has_value() == *test_case.assertion_exists,
                test_case.file + " " + explorer + " assertion-existence mismatch");
    }
}

void require_naive_dpor_agree(const GalleryCase& test_case,
                              const model::CheckResult& naive,
                              const model::CheckResult& dpor) {
    require(naive.schedules_explored < test_case.max_schedules,
            test_case.file + " naive hit max_schedules before exhausting the bound");
    require(dpor.schedules_explored < test_case.max_schedules,
            test_case.file + " DPOR hit max_schedules before exhausting the bound");
    require(verdict_kind(naive) == verdict_kind(dpor),
            test_case.file + " naive and DPOR verdicts disagree");
    require(naive.first_race.has_value() == dpor.first_race.has_value(),
            test_case.file + " race existence disagrees");
    require(naive.first_deadlock.has_value() == dpor.first_deadlock.has_value(),
            test_case.file + " deadlock existence disagrees");
    require(naive.first_error.has_value() == dpor.first_error.has_value(),
            test_case.file + " modeled-error existence disagrees");
    require(naive.first_assertion.has_value() == dpor.first_assertion.has_value(),
            test_case.file + " assertion existence disagrees");
    require((naive.cycles_detected > 0) == (dpor.cycles_detected > 0),
            test_case.file + " cycle existence disagrees");
    require((naive.fair_cycles > 0) == (dpor.fair_cycles > 0),
            test_case.file + " fair-cycle existence disagrees");
    require((naive.unfair_cycles > 0) == (dpor.unfair_cycles > 0),
            test_case.file + " unfair-cycle existence disagrees");
    require((naive.bound_exceeded_executions > 0) == (dpor.bound_exceeded_executions > 0),
            test_case.file + " bound-hit existence disagrees");
    require(dpor.schedules_explored <= naive.schedules_explored,
            test_case.file + " DPOR explored more schedules than naive");
}

void require_replays_report(const std::string& label,
                            const model::ModelChecker& checker,
                            const model::CheckResult& result) {
    if (result.first_race.has_value()) {
        const auto replay = checker.replay(result.first_race->schedule);
        require(replay.first_race.has_value() && *replay.first_race == *result.first_race,
                label + " race report did not replay identically");
    }
    if (result.first_deadlock.has_value()) {
        const auto replay = checker.replay(result.first_deadlock->schedule);
        require(replay.first_deadlock.has_value() && *replay.first_deadlock == *result.first_deadlock,
                label + " deadlock report did not replay identically");
    }
    if (result.first_error.has_value()) {
        const auto replay = checker.replay(result.first_error->schedule);
        require(replay.first_error.has_value() && *replay.first_error == *result.first_error,
                label + " modeled-error report did not replay identically");
    }
    if (result.first_assertion.has_value()) {
        const auto replay = checker.replay(result.first_assertion->schedule);
        require(replay.first_assertion.has_value() && *replay.first_assertion == *result.first_assertion,
                label + " assertion report did not replay identically");
    }
    if (result.first_nontermination.has_value()) {
        const auto replay = checker.replay(result.first_nontermination->schedule);
        require(replay.first_nontermination.has_value() &&
                    *replay.first_nontermination == *result.first_nontermination,
                label + " nontermination report did not replay identically");
    }
}

std::string first_line(const std::string& text) {
    const std::size_t newline = text.find('\n');
    if (newline == std::string::npos) {
        return text;
    }
    return text.substr(0, newline);
}

std::string schedule_block(const std::string& report) {
    const std::string marker = "schedule:\n";
    const std::size_t offset = report.find(marker);
    if (offset == std::string::npos) {
        fail("report did not contain schedule block");
    }
    return report.substr(offset + marker.size());
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
            // also_found summarizes the WHOLE exploration; a replay of one
            // schedule cannot reproduce it, so it is exploration-scope, not
            // part of the reproducible bug report.
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

void require_cli_round_trip(const std::filesystem::path& binary,
                            const std::filesystem::path& source_dir,
                            const std::filesystem::path& work_dir,
                            const GalleryCase& test_case) {
    const std::filesystem::path program = gallery_file(source_dir, test_case.file);
    const std::string stem = std::filesystem::path(test_case.file).stem().string();
    const auto check = run_command(
        binary,
        {"check", program.string(), "--explorer", "dpor",
         "--memory-model", memory_model_argument(test_case.memory_model),
         "--step-bound", std::to_string(test_case.step_bound),
         "--max-schedules", std::to_string(test_case.max_schedules)},
        work_dir / (stem + ".check.out"),
        work_dir / (stem + ".check.err"));

    require(check.exit_code == 1, test_case.file + " CLI check should report a bug");
    require(check.stderr_text.empty(), test_case.file + " CLI check wrote stderr: " + check.stderr_text);
    require(first_line(check.stdout_text) == "verdict: " + expected_text(test_case.expected),
            test_case.file + " CLI check verdict mismatch");

    const std::filesystem::path schedule_path = work_dir / (stem + ".schedule");
    write_file(schedule_path, schedule_block(check.stdout_text));
    const auto replay = run_command(
        binary,
        {"replay", program.string(), "--schedule", schedule_path.string(),
         "--memory-model", memory_model_argument(test_case.memory_model)},
        work_dir / (stem + ".replay.out"),
        work_dir / (stem + ".replay.err"));

    require(replay.exit_code == 1, test_case.file + " CLI replay should report a bug");
    require(replay.stderr_text.empty(), test_case.file + " CLI replay wrote stderr: " + replay.stderr_text);
    require(details_for_round_trip(replay.stdout_text) == details_for_round_trip(check.stdout_text),
            test_case.file + " CLI replay details differed from check report");
}

const std::vector<GalleryCase>& gallery_cases() {
    // Bounds are intentionally small and per-file:
    // - Peterson/Dekker counter models have enough room for completed critical
    //   sections and an exact repeated spin state.
    // - The spawned Peterson assertion model uses a prefix bound; reaching full
    //   completion under the naive oracle is much larger than this regression
    //   suite needs, and the verdict is explicitly "clean up to bound".
    // - Bakery keeps the bounded-ticket simplification honest but low enough
    //   that the naive oracle exhausts before max_schedules.
    // - Treiber has no intended bound hit; failed-CAS handoff does.
    static const std::vector<GalleryCase> cases = {
        // Formerly clean up to bound: bound 10 is sufficient to close the
        // waiting contender's exact Peterson spin cycle.
        {"peterson_counter.dpor", ExpectedVerdict::NonTermination, 10, 300000, false, model::MemoryModel::SC, std::nullopt, false, model::Fairness::UnfairScheduleWitness},
        {"peterson_counter_broken_wrong_flag.dpor", ExpectedVerdict::Race, 8, 100000, true, model::MemoryModel::SC, std::nullopt, false},
        {"peterson_inside_assert.dpor", ExpectedVerdict::CleanUpToBound, 9, 500000, false, model::MemoryModel::SC, std::nullopt, false},
        {"peterson_inside_assert_broken_wrong_flag.dpor", ExpectedVerdict::Race, 9, 1000000, true, model::MemoryModel::SC, std::nullopt, false},
        // Formerly clean up to bound: the existing bound already contains a
        // complete repeated Dekker outer-wait cycle.
        {"dekker_counter.dpor", ExpectedVerdict::NonTermination, 9, 300000, false, model::MemoryModel::SC, std::nullopt, false, model::Fairness::UnfairScheduleWitness},
        {"dekker_counter_broken_drop_turn_wait.dpor", ExpectedVerdict::Race, 10, 500000, true, model::MemoryModel::SC, std::nullopt, false},
        // Formerly clean up to bound: waiting for choosing1 exposes a stable
        // two-step repeated state before the residual bound outcomes.
        {"bakery_bounded_counter.dpor", ExpectedVerdict::NonTermination, 10, 1000000, false, model::MemoryModel::SC, std::nullopt, false, model::Fairness::UnfairScheduleWitness},
        {"bakery_bounded_counter_broken_no_choosing_wait.dpor", ExpectedVerdict::Race, 10, 300000, true, model::MemoryModel::SC, std::nullopt, false},
        {"treiber_push.dpor", ExpectedVerdict::Clean, 12, 100000, false, model::MemoryModel::SC, std::nullopt, false},
        {"treiber_push_broken_load_store.dpor", ExpectedVerdict::Assertion, 12, 100000, true, model::MemoryModel::SC, std::nullopt, false},
        // Formerly clean up to bound: indefinitely postponing ready's release
        // closes the reader's failed-CAS retry cycle.
        {"failed_cas_handoff.dpor", ExpectedVerdict::NonTermination, 10, 100000, false, model::MemoryModel::SC, std::nullopt, false, model::Fairness::UnfairScheduleWitness},
        {"failed_cas_handoff_broken_no_retry.dpor", ExpectedVerdict::Race, 10, 100000, true, model::MemoryModel::SC, std::nullopt, false},
        {"readers_writers.dpor", ExpectedVerdict::Clean, 10, 100000, false, model::MemoryModel::SC, false, false},
        {"readers_writers_broken.dpor", ExpectedVerdict::Race, 10, 100000, true, model::MemoryModel::SC, true, false},
        {"peterson_tso.dpor", ExpectedVerdict::Race, 12, 300000, true, model::MemoryModel::TSO, true, true},
        {"peterson_tso_fenced.dpor", ExpectedVerdict::Race, 13, 300000, false, model::MemoryModel::TSO, false, true},
        // The unfenced PSO run reaches both race and assertion witnesses but
        // remains capped at this gallery budget; existence is still sound.
        // The fenced PSO run exhausts and proves assertion absence.
        {"peterson_tso.dpor", ExpectedVerdict::Race, 10, 300000, false, model::MemoryModel::PSO, true, true},
        {"peterson_tso_fenced.dpor", ExpectedVerdict::Race, 13, 300000, false, model::MemoryModel::PSO, false, true},
        {"dekker_tso.dpor", ExpectedVerdict::Race, 14, 500000, true, model::MemoryModel::TSO, true, true},
        {"dekker_tso_fenced.dpor", ExpectedVerdict::Race, 15, 500000, false, model::MemoryModel::TSO, false, true},
        {"mp_pso.dpor", ExpectedVerdict::Race, 20, 100000, false, model::MemoryModel::SC, false, false},
        {"mp_pso.dpor", ExpectedVerdict::Race, 20, 100000, false, model::MemoryModel::TSO, false, false},
        {"mp_pso.dpor", ExpectedVerdict::Race, 20, 100000, false, model::MemoryModel::PSO, true, false},
        {"mp_pso_fenced.dpor", ExpectedVerdict::Race, 20, 100000, false, model::MemoryModel::SC, false, false},
        {"mp_pso_fenced.dpor", ExpectedVerdict::Race, 20, 100000, false, model::MemoryModel::TSO, false, false},
        {"mp_pso_fenced.dpor", ExpectedVerdict::Race, 20, 100000, false, model::MemoryModel::PSO, false, false},
    };
    return cases;
}

void verify_gallery_with_library(const std::filesystem::path& source_dir) {
    for (const GalleryCase& test_case : gallery_cases()) {
        const model::Program program = cli::parse_program_file(gallery_file(source_dir, test_case.file).string());
        const model::ModelChecker checker(program, test_case.step_bound, test_case.memory_model);
        const model::CheckResult dpor = checker.explore_dpor(test_case.max_schedules);

        require_expected_verdict(test_case, dpor, "DPOR");
        if (!test_case.dpor_only) {
            const model::CheckResult naive = checker.explore_naive(test_case.max_schedules);
            require_expected_verdict(test_case, naive, "naive");
            require_naive_dpor_agree(test_case, naive, dpor);
            require_replays_report(test_case.file + " naive", checker, naive);
        }
        require_replays_report(test_case.file + " DPOR", checker, dpor);
    }
}

void verify_broken_variants_with_cli(const std::filesystem::path& binary,
                                     const std::filesystem::path& source_dir,
                                     const std::filesystem::path& work_dir) {
    for (const GalleryCase& test_case : gallery_cases()) {
        if (test_case.broken) {
            require_cli_round_trip(binary, source_dir, work_dir, test_case);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        fail("usage: classic_gallery_tests <dpor-binary> <source-dir>");
    }

    const std::filesystem::path binary = argv[1];
    const std::filesystem::path source_dir = argv[2];
    const std::filesystem::path work_dir = std::filesystem::current_path();

    verify_gallery_with_library(source_dir);
    verify_broken_variants_with_cli(binary, source_dir, work_dir);
    return 0;
}
