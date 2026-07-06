#include "program_parser.hpp"
#include "report.hpp"

#include "model/checker.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

class UsageError : public std::runtime_error {
public:
    explicit UsageError(const std::string& message) : std::runtime_error(message) {}
};

enum class Explorer { Naive, Dpor };

struct CheckCommand {
    std::string program_path;
    Explorer explorer{Explorer::Dpor};
    std::size_t max_schedules{100000};
};

struct ReplayCommand {
    std::string program_path;
    std::string schedule_path;
};

std::size_t parse_size_token(const std::string& token, const std::string& description) {
    if (token.empty()) {
        throw UsageError("missing " + description);
    }

    std::uint64_t value = 0;
    for (const char character : token) {
        if (character < '0' || character > '9') {
            throw UsageError("invalid " + description);
        }
        value = value * 10 + static_cast<std::uint64_t>(character - '0');
        if (value > std::numeric_limits<std::size_t>::max()) {
            throw UsageError(description + " is too large");
        }
    }
    if (value == 0) {
        throw UsageError(description + " must be greater than zero");
    }
    return static_cast<std::size_t>(value);
}

Explorer parse_explorer(const std::string& value) {
    if (value == "naive") {
        return Explorer::Naive;
    }
    if (value == "dpor") {
        return Explorer::Dpor;
    }
    throw UsageError("invalid explorer");
}

CheckCommand parse_check_command(int argc, char** argv) {
    if (argc < 3) {
        throw UsageError("usage: dpor check <program.dpor> [--explorer naive|dpor] [--max-schedules N]");
    }

    CheckCommand command;
    command.program_path = argv[2];
    int index = 3;
    while (index < argc) {
        const std::string flag = argv[index];
        if (flag == "--explorer") {
            if (index + 1 >= argc) {
                throw UsageError("missing explorer");
            }
            command.explorer = parse_explorer(argv[index + 1]);
            index += 2;
        } else if (flag == "--max-schedules") {
            if (index + 1 >= argc) {
                throw UsageError("missing max schedules");
            }
            command.max_schedules = parse_size_token(argv[index + 1], "max schedules");
            index += 2;
        } else {
            throw UsageError("unknown option");
        }
    }
    return command;
}

ReplayCommand parse_replay_command(int argc, char** argv) {
    if (argc != 5) {
        throw UsageError("usage: dpor replay <program.dpor> --schedule <schedule-file>");
    }
    if (std::string(argv[3]) != "--schedule") {
        throw UsageError("usage: dpor replay <program.dpor> --schedule <schedule-file>");
    }
    return ReplayCommand{argv[2], argv[4]};
}

int run_check(const CheckCommand& command) {
    const model::Program program = cli::parse_program_file(command.program_path);
    const model::ModelChecker checker(program);
    const model::CheckResult result = command.explorer == Explorer::Naive
        ? checker.explore_naive(command.max_schedules)
        : checker.explore_dpor(command.max_schedules);
    cli::print_report(std::cout, program, result);
    return cli::has_bug(result) ? 1 : 0;
}

int run_replay(const ReplayCommand& command) {
    const model::Program program = cli::parse_program_file(command.program_path);
    const model::Schedule schedule = cli::parse_schedule_file(command.schedule_path);
    const model::ModelChecker checker(program);
    const model::CheckResult result = checker.replay(schedule);
    cli::print_report(std::cout, program, result);
    return cli::has_bug(result) ? 1 : 0;
}

void print_parse_error(const cli::ParseError& error) {
    if (error.line() == 0) {
        std::cerr << "parse error: " << error.what() << '\n';
    } else {
        std::cerr << "parse error at line " << error.line() << ": " << error.what() << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            throw UsageError("usage: dpor <check|replay> ...");
        }

        const std::string subcommand = argv[1];
        if (subcommand == "check") {
            return run_check(parse_check_command(argc, argv));
        }
        if (subcommand == "replay") {
            return run_replay(parse_replay_command(argc, argv));
        }
        throw UsageError("usage: dpor <check|replay> ...");
    } catch (const cli::ParseError& error) {
        print_parse_error(error);
        return 2;
    } catch (const std::invalid_argument& error) {
        std::cerr << error.what() << '\n';
        return 2;
    } catch (const UsageError& error) {
        std::cerr << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
