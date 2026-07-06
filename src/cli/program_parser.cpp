#include "program_parser.hpp"

#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

namespace cli {
namespace {

std::string trim(const std::string& input) {
    const std::size_t first = input.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const std::size_t last = input.find_last_not_of(" \t\r\n");
    return input.substr(first, last - first + 1);
}

std::string strip_comment(const std::string& input) {
    const std::size_t marker = input.find('#');
    if (marker == std::string::npos) {
        return input;
    }
    return input.substr(0, marker);
}

std::vector<std::string> split_words(const std::string& input) {
    std::istringstream stream(input);
    std::vector<std::string> words;
    std::string word;
    while (stream >> word) {
        words.push_back(std::move(word));
    }
    return words;
}

std::uint32_t parse_u32_token(const std::string& token,
                              std::size_t line,
                              const std::string& description) {
    if (token.empty()) {
        throw ParseError(line, "expected " + description);
    }
    std::uint64_t value = 0;
    for (const char character : token) {
        if (character < '0' || character > '9') {
            throw ParseError(line, "invalid " + description + " '" + token + "'");
        }
        value = value * 10 + static_cast<std::uint64_t>(character - '0');
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            throw ParseError(line, description + " is too large");
        }
    }
    return static_cast<std::uint32_t>(value);
}

void require_arity(const std::vector<std::string>& words,
                   std::size_t expected,
                   std::size_t line,
                   const std::string& keyword) {
    if (words.size() != expected) {
        std::ostringstream message;
        message << "keyword '" << keyword << "' expects " << (expected - 1)
                << " operand";
        if (expected != 2) {
            message << "s";
        }
        throw ParseError(line, message.str());
    }
}

model::Action memory_action(model::ActionKind kind, const std::string& address) {
    model::Action action;
    action.kind = kind;
    action.address = address;
    return action;
}

model::Action mutex_action(model::ActionKind kind, const std::string& mutex) {
    model::Action action;
    action.kind = kind;
    action.mutex = mutex;
    return action;
}

model::Action condition_action(model::ActionKind kind, const std::string& condition) {
    model::Action action;
    action.kind = kind;
    action.condition = condition;
    return action;
}

model::Action wait_action(const std::string& condition, const std::string& mutex) {
    model::Action action;
    action.kind = model::ActionKind::Wait;
    action.condition = condition;
    action.mutex = mutex;
    return action;
}

model::Action join_action(model::ThreadId target) {
    model::Action action;
    action.kind = model::ActionKind::Join;
    action.target = target;
    return action;
}

model::Action yield_action() {
    model::Action action;
    action.kind = model::ActionKind::Yield;
    return action;
}

model::Action parse_action(const std::vector<std::string>& words, std::size_t line) {
    const std::string& keyword = words.front();
    if (keyword == "read") {
        require_arity(words, 2, line, keyword);
        return memory_action(model::ActionKind::Read, words[1]);
    }
    if (keyword == "write") {
        require_arity(words, 2, line, keyword);
        return memory_action(model::ActionKind::Write, words[1]);
    }
    if (keyword == "atomic_load") {
        require_arity(words, 2, line, keyword);
        return memory_action(model::ActionKind::AtomicLoad, words[1]);
    }
    if (keyword == "atomic_store") {
        require_arity(words, 2, line, keyword);
        return memory_action(model::ActionKind::AtomicStore, words[1]);
    }
    if (keyword == "atomic_rmw") {
        require_arity(words, 2, line, keyword);
        return memory_action(model::ActionKind::AtomicRmw, words[1]);
    }
    if (keyword == "lock") {
        require_arity(words, 2, line, keyword);
        return mutex_action(model::ActionKind::Lock, words[1]);
    }
    if (keyword == "unlock") {
        require_arity(words, 2, line, keyword);
        return mutex_action(model::ActionKind::Unlock, words[1]);
    }
    if (keyword == "wait") {
        require_arity(words, 3, line, keyword);
        return wait_action(words[1], words[2]);
    }
    if (keyword == "signal") {
        require_arity(words, 2, line, keyword);
        return condition_action(model::ActionKind::Signal, words[1]);
    }
    if (keyword == "broadcast") {
        require_arity(words, 2, line, keyword);
        return condition_action(model::ActionKind::Broadcast, words[1]);
    }
    if (keyword == "join") {
        require_arity(words, 2, line, keyword);
        return join_action(parse_u32_token(words[1], line, "join target"));
    }
    if (keyword == "yield") {
        require_arity(words, 1, line, keyword);
        return yield_action();
    }

    throw ParseError(line, "unknown keyword '" + keyword + "'");
}

std::ifstream open_input_file(const std::string& path, const std::string& kind) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("could not open " + kind + " file");
    }
    return input;
}

} // namespace

ParseError::ParseError(std::size_t line, const std::string& message)
    : std::runtime_error(message), line_(line) {}

std::size_t ParseError::line() const {
    return line_;
}

model::Program parse_program_file(const std::string& path) {
    std::ifstream input = open_input_file(path, "program");
    model::Program program;
    std::vector<std::pair<std::size_t, model::ThreadId>> joins;
    std::size_t current_thread = std::numeric_limits<std::size_t>::max();

    std::string line_text;
    std::size_t line = 0;
    while (std::getline(input, line_text)) {
        ++line;
        const std::string stripped = trim(strip_comment(line_text));
        if (stripped.empty()) {
            continue;
        }

        const std::vector<std::string> words = split_words(stripped);
        if (words.empty()) {
            continue;
        }

        if (words.front() == "thread:") {
            if (words.size() != 1) {
                throw ParseError(line, "thread declaration takes no operands");
            }
            program.threads.emplace_back();
            current_thread = program.threads.size() - 1;
            continue;
        }

        if (current_thread == std::numeric_limits<std::size_t>::max()) {
            throw ParseError(line, "action appears before first thread declaration");
        }

        model::Action action = parse_action(words, line);
        if (action.kind == model::ActionKind::Join) {
            joins.emplace_back(line, action.target);
        }
        program.threads.at(current_thread).push_back(std::move(action));
    }

    if (program.threads.empty()) {
        throw ParseError(0, "program declares no threads");
    }

    for (const auto& [join_line, target] : joins) {
        if (target >= program.threads.size()) {
            std::ostringstream message;
            message << "join target " << target << " is not declared";
            throw ParseError(join_line, message.str());
        }
    }

    return program;
}

model::Schedule parse_schedule_file(const std::string& path) {
    std::ifstream input = open_input_file(path, "schedule");
    model::Schedule schedule;

    std::string line_text;
    std::size_t line = 0;
    while (std::getline(input, line_text)) {
        ++line;
        const std::string stripped = trim(strip_comment(line_text));
        if (stripped.empty()) {
            continue;
        }

        const std::vector<std::string> words = split_words(stripped);
        if (words.size() != 2) {
            throw ParseError(line, "schedule step must be two integers");
        }
        schedule.push_back(model::ScheduleStep{
            parse_u32_token(words[0], line, "thread id"),
            parse_u32_token(words[1], line, "action index"),
        });
    }

    return schedule;
}

} // namespace cli
