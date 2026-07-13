#include "program_parser.hpp"

#include <cstdint>
#include <exception>
#include <fstream>
#include <istream>
#include <limits>
#include <map>
#include <sstream>
#include <utility>
#include <vector>

namespace cli {
namespace {

struct SpawnReference {
    std::size_t line{0};
    model::ThreadId source{0};
    model::ThreadId target{0};
};

struct BranchReference {
    std::size_t line{0};
    model::ThreadId source{0};
    std::string label;
};

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

model::Value parse_i64_token(const std::string& token,
                             std::size_t line,
                             const std::string& description) {
    try {
        std::size_t parsed = 0;
        const long long value = std::stoll(token, &parsed, 10);
        if (parsed != token.size()) {
            throw ParseError(line, "invalid " + description + " '" + token + "'");
        }
        return static_cast<model::Value>(value);
    } catch (const std::invalid_argument&) {
        throw ParseError(line, "invalid " + description + " '" + token + "'");
    } catch (const std::out_of_range&) {
        throw ParseError(line, description + " is too large");
    }
}

model::RegisterId parse_register_token(const std::string& token,
                                        std::size_t line,
                                        const std::string& description) {
    if (token.size() != 2 || token[0] != 'r' || token[1] < '0' || token[1] > '9') {
        throw ParseError(line, "invalid " + description + " '" + token + "'");
    }
    const auto reg = static_cast<model::RegisterId>(token[1] - '0');
    if (reg >= model::kRegisterCount) {
        throw ParseError(line, description + " is out of range");
    }
    return reg;
}

model::ValueOperand immediate_operand(model::Value value) {
    model::ValueOperand operand;
    operand.kind = model::ValueOperandKind::Immediate;
    operand.immediate = value;
    return operand;
}

model::ValueOperand register_operand(model::RegisterId reg) {
    model::ValueOperand operand;
    operand.kind = model::ValueOperandKind::Register;
    operand.reg = reg;
    return operand;
}

model::ValueOperand parse_value_operand(const std::string& token, std::size_t line) {
    if (!token.empty() && token.front() == 'r') {
        return register_operand(parse_register_token(token, line, "register operand"));
    }
    return immediate_operand(parse_i64_token(token, line, "integer operand"));
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

model::Action memory_read_action(model::ActionKind kind,
                                 const std::string& address,
                                 std::optional<model::RegisterId> destination) {
    model::Action action = memory_action(kind, address);
    action.destination = destination;
    return action;
}

model::Action memory_write_action(model::ActionKind kind,
                                  const std::string& address,
                                  std::optional<model::ValueOperand> value) {
    model::Action action = memory_action(kind, address);
    action.value = value;
    return action;
}

model::Action atomic_rmw_action(const std::string& address,
                                std::optional<model::ValueOperand> value,
                                std::optional<model::RegisterId> destination) {
    model::Action action = memory_action(model::ActionKind::AtomicRmw, address);
    action.value = value;
    action.destination = destination;
    return action;
}

model::Action cas_action(const std::string& address,
                         model::ValueOperand expected,
                         model::ValueOperand desired,
                         model::RegisterId destination) {
    model::Action action = memory_action(model::ActionKind::CompareExchange, address);
    action.expected = expected;
    action.value = desired;
    action.destination = destination;
    return action;
}

model::Action mutex_action(model::ActionKind kind, const std::string& mutex) {
    model::Action action;
    action.kind = kind;
    action.mutex = mutex;
    return action;
}

model::Action rwlock_action(model::ActionKind kind, const std::string& rwlock) {
    model::Action action;
    action.kind = kind;
    action.rwlock = rwlock;
    return action;
}

model::Action semaphore_action(model::ActionKind kind, const std::string& semaphore) {
    model::Action action;
    action.kind = kind;
    action.semaphore = semaphore;
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

model::Action spawn_action(model::ThreadId target) {
    model::Action action;
    action.kind = model::ActionKind::Spawn;
    action.target = target;
    return action;
}

model::Action yield_action() {
    model::Action action;
    action.kind = model::ActionKind::Yield;
    return action;
}

model::Action fence_action() {
    model::Action action;
    action.kind = model::ActionKind::Fence;
    return action;
}

model::Action set_action(model::RegisterId destination, model::Value value) {
    model::Action action;
    action.kind = model::ActionKind::Set;
    action.destination = destination;
    action.value = immediate_operand(value);
    return action;
}

model::Action branch_action(model::RegisterId source, std::string label) {
    model::Action action;
    action.kind = model::ActionKind::BranchNonzero;
    action.source_register = source;
    action.label = std::move(label);
    return action;
}

model::Action assert_action(model::RegisterId source) {
    model::Action action;
    action.kind = model::ActionKind::Assert;
    action.source_register = source;
    return action;
}

model::Action label_action(std::string label) {
    model::Action action;
    action.kind = model::ActionKind::Label;
    action.label = std::move(label);
    return action;
}

bool is_line_label(const std::vector<std::string>& words) {
    return words.size() == 1 &&
           words.front().size() > 1 &&
           words.front().back() == ':';
}

std::string line_label_name(const std::string& token) {
    return token.substr(0, token.size() - 1);
}

model::Action parse_action(const std::vector<std::string>& words, std::size_t line) {
    const std::string& keyword = words.front();
    if (is_line_label(words)) {
        return label_action(line_label_name(keyword));
    }
    if (keyword == "set") {
        require_arity(words, 3, line, keyword);
        return set_action(parse_register_token(words[1], line, "destination register"),
                          parse_i64_token(words[2], line, "set immediate"));
    }
    if (keyword == "label") {
        require_arity(words, 2, line, keyword);
        return label_action(words[1]);
    }
    if (keyword == "bnz") {
        require_arity(words, 3, line, keyword);
        return branch_action(parse_register_token(words[1], line, "branch register"), words[2]);
    }
    if (keyword == "assert") {
        require_arity(words, 2, line, keyword);
        return assert_action(parse_register_token(words[1], line, "assert register"));
    }
    if (keyword == "read") {
        if (words.size() == 2) {
            return memory_read_action(model::ActionKind::Read, words[1], std::nullopt);
        }
        if (words.size() == 4 && words[2] == "->") {
            return memory_read_action(
                model::ActionKind::Read,
                words[1],
                parse_register_token(words[3], line, "destination register"));
        }
        throw ParseError(line, "keyword 'read' expects address or address -> register");
    }
    if (keyword == "write") {
        if (words.size() == 2) {
            return memory_write_action(model::ActionKind::Write, words[1], std::nullopt);
        }
        if (words.size() == 3) {
            return memory_write_action(model::ActionKind::Write, words[1], parse_value_operand(words[2], line));
        }
        throw ParseError(line, "keyword 'write' expects address or address value");
    }
    if (keyword == "atomic_load") {
        if (words.size() == 2) {
            return memory_read_action(model::ActionKind::AtomicLoad, words[1], std::nullopt);
        }
        if (words.size() == 4 && words[2] == "->") {
            return memory_read_action(
                model::ActionKind::AtomicLoad,
                words[1],
                parse_register_token(words[3], line, "destination register"));
        }
        throw ParseError(line, "keyword 'atomic_load' expects address or address -> register");
    }
    if (keyword == "atomic_store") {
        if (words.size() == 2) {
            return memory_write_action(model::ActionKind::AtomicStore, words[1], std::nullopt);
        }
        if (words.size() == 3) {
            return memory_write_action(model::ActionKind::AtomicStore, words[1], parse_value_operand(words[2], line));
        }
        throw ParseError(line, "keyword 'atomic_store' expects address or address value");
    }
    if (keyword == "atomic_rmw") {
        if (words.size() == 2) {
            return atomic_rmw_action(words[1], std::nullopt, std::nullopt);
        }
        if (words.size() == 5 && words[3] == "->") {
            return atomic_rmw_action(
                words[1],
                parse_value_operand(words[2], line),
                parse_register_token(words[4], line, "destination register"));
        }
        throw ParseError(line, "keyword 'atomic_rmw' expects address or address value -> register");
    }
    if (keyword == "cas") {
        require_arity(words, 6, line, keyword);
        if (words[4] != "->") {
            throw ParseError(line, "keyword 'cas' expects address expected desired -> register");
        }
        return cas_action(words[1],
                          parse_value_operand(words[2], line),
                          parse_value_operand(words[3], line),
                          parse_register_token(words[5], line, "destination register"));
    }
    if (keyword == "fence") {
        require_arity(words, 1, line, keyword);
        return fence_action();
    }
    if (keyword == "lock") {
        require_arity(words, 2, line, keyword);
        return mutex_action(model::ActionKind::Lock, words[1]);
    }
    if (keyword == "unlock") {
        require_arity(words, 2, line, keyword);
        return mutex_action(model::ActionKind::Unlock, words[1]);
    }
    if (keyword == "rlock") {
        require_arity(words, 2, line, keyword);
        return rwlock_action(model::ActionKind::RLock, words[1]);
    }
    if (keyword == "runlock") {
        require_arity(words, 2, line, keyword);
        return rwlock_action(model::ActionKind::RUnlock, words[1]);
    }
    if (keyword == "wlock") {
        require_arity(words, 2, line, keyword);
        return rwlock_action(model::ActionKind::WLock, words[1]);
    }
    if (keyword == "wunlock") {
        require_arity(words, 2, line, keyword);
        return rwlock_action(model::ActionKind::WUnlock, words[1]);
    }
    if (keyword == "sem_post") {
        require_arity(words, 2, line, keyword);
        return semaphore_action(model::ActionKind::SemPost, words[1]);
    }
    if (keyword == "sem_wait") {
        require_arity(words, 2, line, keyword);
        return semaphore_action(model::ActionKind::SemWait, words[1]);
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
    if (keyword == "spawn") {
        require_arity(words, 2, line, keyword);
        return spawn_action(parse_u32_token(words[1], line, "spawn target"));
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

model::Program parse_program_stream(std::istream& input) {
    model::Program program;
    std::vector<std::pair<std::size_t, model::ThreadId>> joins;
    std::vector<SpawnReference> spawns;
    std::vector<BranchReference> branches;
    std::vector<std::map<std::string, std::size_t>> labels;
    std::map<std::string, std::size_t> mutex_first_use;
    std::map<std::string, std::size_t> rwlock_first_use;
    std::map<std::string, std::size_t> semaphore_first_use;
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
            labels.emplace_back();
            current_thread = program.threads.size() - 1;
            continue;
        }

        if (current_thread == std::numeric_limits<std::size_t>::max()) {
            throw ParseError(line, "action appears before first thread declaration");
        }

        model::Action action = parse_action(words, line);
        const bool uses_mutex = action.kind == model::ActionKind::Lock ||
                                action.kind == model::ActionKind::Unlock ||
                                action.kind == model::ActionKind::Wait;
        const bool uses_rwlock = action.kind == model::ActionKind::RLock ||
                                 action.kind == model::ActionKind::RUnlock ||
                                 action.kind == model::ActionKind::WLock ||
                                 action.kind == model::ActionKind::WUnlock;
        const bool uses_semaphore = action.kind == model::ActionKind::SemPost ||
                                    action.kind == model::ActionKind::SemWait;
        if (uses_mutex) {
            const auto rwlock_use = rwlock_first_use.find(action.mutex);
            if (rwlock_use != rwlock_first_use.end()) {
                std::ostringstream message;
                message << "name '" << action.mutex
                        << "' is already used as an rwlock on line " << rwlock_use->second
                        << " and cannot also be used as a mutex";
                throw ParseError(line, message.str());
            }
            const auto semaphore_use = semaphore_first_use.find(action.mutex);
            if (semaphore_use != semaphore_first_use.end()) {
                std::ostringstream message;
                message << "name '" << action.mutex
                        << "' is already used as a semaphore on line " << semaphore_use->second
                        << " and cannot also be used as a mutex";
                throw ParseError(line, message.str());
            }
            mutex_first_use.emplace(action.mutex, line);
        }
        if (uses_rwlock) {
            const auto mutex_use = mutex_first_use.find(action.rwlock);
            if (mutex_use != mutex_first_use.end()) {
                std::ostringstream message;
                message << "name '" << action.rwlock
                        << "' is already used as a mutex on line " << mutex_use->second
                        << " and cannot also be used as an rwlock";
                throw ParseError(line, message.str());
            }
            const auto semaphore_use = semaphore_first_use.find(action.rwlock);
            if (semaphore_use != semaphore_first_use.end()) {
                std::ostringstream message;
                message << "name '" << action.rwlock
                        << "' is already used as a semaphore on line " << semaphore_use->second
                        << " and cannot also be used as an rwlock";
                throw ParseError(line, message.str());
            }
            rwlock_first_use.emplace(action.rwlock, line);
        }
        if (uses_semaphore) {
            const auto mutex_use = mutex_first_use.find(action.semaphore);
            if (mutex_use != mutex_first_use.end()) {
                std::ostringstream message;
                message << "name '" << action.semaphore
                        << "' is already used as a mutex on line " << mutex_use->second
                        << " and cannot also be used as a semaphore";
                throw ParseError(line, message.str());
            }
            const auto rwlock_use = rwlock_first_use.find(action.semaphore);
            if (rwlock_use != rwlock_first_use.end()) {
                std::ostringstream message;
                message << "name '" << action.semaphore
                        << "' is already used as an rwlock on line " << rwlock_use->second
                        << " and cannot also be used as a semaphore";
                throw ParseError(line, message.str());
            }
            semaphore_first_use.emplace(action.semaphore, line);
        }
        if (action.kind == model::ActionKind::Join) {
            joins.emplace_back(line, action.target);
        } else if (action.kind == model::ActionKind::Spawn) {
            spawns.push_back(SpawnReference{
                line,
                static_cast<model::ThreadId>(current_thread),
                action.target,
            });
        } else if (action.kind == model::ActionKind::Label) {
            auto& thread_labels = labels.at(current_thread);
            if (thread_labels.find(action.label) != thread_labels.end()) {
                throw ParseError(line, "label '" + action.label + "' is already declared in this thread");
            }
            thread_labels.emplace(action.label, line);
        } else if (action.kind == model::ActionKind::BranchNonzero) {
            branches.push_back(BranchReference{
                line,
                static_cast<model::ThreadId>(current_thread),
                action.label,
            });
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
    for (const BranchReference& branch : branches) {
        if (labels.at(branch.source).find(branch.label) == labels.at(branch.source).end()) {
            throw ParseError(branch.line, "branch target label '" + branch.label + "' is not declared in this thread");
        }
    }
    std::vector<std::size_t> first_spawn_line(program.threads.size(), 0);
    for (const SpawnReference& spawn : spawns) {
        if (spawn.target >= program.threads.size()) {
            std::ostringstream message;
            message << "spawn target " << spawn.target << " is not declared";
            throw ParseError(spawn.line, message.str());
        }
        if (spawn.target == spawn.source) {
            std::ostringstream message;
            message << "spawn target " << spawn.target << " is self";
            throw ParseError(spawn.line, message.str());
        }
        if (first_spawn_line.at(spawn.target) != 0) {
            std::ostringstream message;
            message << "spawn target " << spawn.target << " is targeted by more than one spawn";
            throw ParseError(spawn.line, message.str());
        }
        first_spawn_line.at(spawn.target) = spawn.line;
    }

    return program;
}

std::string register_text(model::RegisterId reg) {
    std::ostringstream output;
    output << 'r' << static_cast<unsigned>(reg);
    return output.str();
}

std::string operand_text(const model::ValueOperand& operand) {
    std::ostringstream output;
    if (operand.kind == model::ValueOperandKind::Register) {
        output << register_text(operand.reg);
    } else {
        output << operand.immediate;
    }
    return output.str();
}

} // namespace

ParseError::ParseError(std::size_t line, const std::string& message)
    : std::runtime_error(message), line_(line) {}

std::size_t ParseError::line() const {
    return line_;
}

std::string action_text(const model::Action& action) {
    std::ostringstream output;
    switch (action.kind) {
    case model::ActionKind::Set:
        output << "set " << register_text(action.destination.value_or(0)) << ' '
               << operand_text(action.value.value_or(immediate_operand(0)));
        break;
    case model::ActionKind::Label:
        output << "label " << action.label;
        break;
    case model::ActionKind::BranchNonzero:
        output << "bnz " << register_text(action.source_register.value_or(0)) << ' '
               << action.label;
        break;
    case model::ActionKind::Assert:
        output << "assert " << register_text(action.source_register.value_or(0));
        break;
    case model::ActionKind::Read:
        output << "read " << action.address;
        if (action.destination.has_value()) {
            output << " -> " << register_text(*action.destination);
        }
        break;
    case model::ActionKind::Write:
        output << "write " << action.address;
        if (action.value.has_value()) {
            output << ' ' << operand_text(*action.value);
        }
        break;
    case model::ActionKind::AtomicLoad:
        output << "atomic_load " << action.address;
        if (action.destination.has_value()) {
            output << " -> " << register_text(*action.destination);
        }
        break;
    case model::ActionKind::AtomicStore:
        output << "atomic_store " << action.address;
        if (action.value.has_value()) {
            output << ' ' << operand_text(*action.value);
        }
        break;
    case model::ActionKind::AtomicRmw:
        output << "atomic_rmw " << action.address;
        if (action.value.has_value() && action.destination.has_value()) {
            output << ' ' << operand_text(*action.value)
                   << " -> " << register_text(*action.destination);
        }
        break;
    case model::ActionKind::CompareExchange:
        output << "cas " << action.address << ' '
               << operand_text(action.expected.value_or(immediate_operand(0))) << ' '
               << operand_text(action.value.value_or(immediate_operand(0))) << " -> "
               << register_text(action.destination.value_or(0));
        break;
    case model::ActionKind::Fence:
        output << "fence";
        break;
    case model::ActionKind::Flush:
        output << "flush " << action.address;
        break;
    case model::ActionKind::Lock:
        output << "lock " << action.mutex;
        break;
    case model::ActionKind::Unlock:
        output << "unlock " << action.mutex;
        break;
    case model::ActionKind::RLock:
        output << "rlock " << action.rwlock;
        break;
    case model::ActionKind::RUnlock:
        output << "runlock " << action.rwlock;
        break;
    case model::ActionKind::WLock:
        output << "wlock " << action.rwlock;
        break;
    case model::ActionKind::WUnlock:
        output << "wunlock " << action.rwlock;
        break;
    case model::ActionKind::SemPost:
        output << "sem_post " << action.semaphore;
        break;
    case model::ActionKind::SemWait:
        output << "sem_wait " << action.semaphore;
        break;
    case model::ActionKind::Spawn:
        output << "spawn " << action.target;
        break;
    case model::ActionKind::Join:
        output << "join " << action.target;
        break;
    case model::ActionKind::Wait:
        output << "wait " << action.condition << " " << action.mutex;
        break;
    case model::ActionKind::Signal:
        output << "signal " << action.condition;
        break;
    case model::ActionKind::Broadcast:
        output << "broadcast " << action.condition;
        break;
    case model::ActionKind::Yield:
        output << "yield";
        break;
    }
    return output.str();
}

model::Program parse_program_file(const std::string& path) {
    std::ifstream input = open_input_file(path, "program");
    return parse_program_stream(input);
}

model::Program parse_program_text(const std::string& text) {
    std::istringstream input(text);
    return parse_program_stream(input);
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
        if (words.size() != 2 && words.size() != 3) {
            throw ParseError(line, "schedule step must be two or three integers");
        }
        const std::optional<std::uint32_t> flush_address = words.size() == 3
            ? std::optional<std::uint32_t>{parse_u32_token(words[2], line, "flush address id")}
            : std::nullopt;
        schedule.push_back(model::ScheduleStep{
            parse_u32_token(words[0], line, "thread id"),
            parse_u32_token(words[1], line, "action index"),
            flush_address,
        });
    }

    return schedule;
}

std::string render_program(const model::Program& program) {
    std::ostringstream output;
    for (const auto& thread : program.threads) {
        output << "thread:\n";
        for (const auto& action : thread) {
            output << "  " << action_text(action) << '\n';
        }
    }
    return output.str();
}

} // namespace cli
